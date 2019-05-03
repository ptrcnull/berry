#include "be_code.h"
#include "be_parser.h"
#include "be_lexer.h"
#include "be_opcode.h"
#include "be_vector.h"
#include "be_list.h"
#include "be_var.h"
#include <stdio.h>

#define min(a, b)               ((a) < (b) ? (a) : (b))
#define exp2anyreg(f, e)        exp2reg(f, e, (f)->freereg)
#define var2anyreg(f, e)        var2reg(f, e, (f)->freereg)
#define hasjump(e)              ((e)->t != (e)->f || (e)->not)
#define code_bool(f, r, b, j)   codeABC(f, OP_LDBOOL, r, b, j)
#define code_move(f, a, b)      codeABC(f, OP_MOVE, a, b, 0)
#define code_call(f, a, b)      codeABC(f, OP_CALL, a, b, 0)
#define code_getmbr(f, a, b, c) codeABC(f, OP_GETMBR, a, b, c)
#define jumpboolop(e, b)        ((b) ^ (e)->not ? OP_JMPT : OP_JMPF)

static int var2reg(bfuncinfo *finfo, bexpdesc *e, int dst);

#if BE_RUNTIME_DEBUG_INFO
static void codelineinfo(bfuncinfo *finfo)
{
    bvector *vec = &finfo->linevec;
    int line = finfo->lexer->lastline;
    blineinfo *li = be_vector_end(vec);
    if (be_vector_isempty(vec) || li->linenumber != line) {
        be_vector_append(vec, NULL);
        li = be_vector_end(vec);
        li->endpc = finfo->pc;
        li->linenumber = line;
        finfo->proto->lineinfo = be_vector_data(vec);
        finfo->proto->nlineinfo = be_vector_count(vec);
    }
}
#else
    #define codelineinfo(finfo)
#endif

static int codeinst(bfuncinfo *finfo, binstruction ins)
{
    /* put new instruction in code array */
    be_vector_append(&finfo->code, &ins);
    finfo->proto->code = be_vector_data(&finfo->code);
    codelineinfo(finfo);
    return finfo->pc++;
}

static int codeABC(bfuncinfo *finfo, bopcode op, int a, int b, int c)
{
    return codeinst(finfo, ISET_OP(op)
        | ISET_RA(a) | ISET_RKB(b) | ISET_RKC(c));
}

static int codeABx(bfuncinfo *finfo, bopcode op, int a, int bx)
{
    return codeinst(finfo, ISET_OP(op) | ISET_RA(a) | ISET_Bx(bx));
}

static void free_expreg(bfuncinfo *finfo, bexpdesc *e)
{
    /* release temporary register */
    if (e->type == ETREG) {
        be_code_freeregs(finfo, 1);
    }
}

static void allocstack(bfuncinfo *finfo, int count)
{
    int nstack = finfo->freereg + count;
    if (nstack > finfo->nstack) {
        finfo->nstack = (bbyte)nstack;
    }
}

int be_code_allocregs(bfuncinfo *finfo, int count)
{
    int base = finfo->freereg;
    allocstack(finfo, count);
    finfo->freereg += (char)count;
    return base;
}

static void setjump(bfuncinfo *finfo, int pc, int dst)
{
    binstruction *p = be_vector_at(&finfo->code, pc);
    int offset = dst - (pc + 1);
    /* instruction edit jump destination */
    *p = (*p & ~IBx_MASK) | ISET_sBx(offset);
}

static int isjumpbool(bfuncinfo *finfo, int pc)
{
    binstruction *p = be_vector_at(&finfo->code, pc);
    bopcode op = IGET_OP(*p);

    if (op == OP_JMPT || op == OP_JMPF) {
        return 1;
    }
    return 0;
}

static int get_jump(bfuncinfo *finfo, int pc)
{
    binstruction *i = be_vector_at(&finfo->code, pc);
    int offset = IGET_sBx(*i);
    return offset == NO_JUMP ? NO_JUMP : pc + 1 + offset;
}

static void patchlistaux(bfuncinfo *finfo, int list, int vtarget, int dtarget)
{
    while (list != NO_JUMP) {
        int next = get_jump(finfo, list);
        if (isjumpbool(finfo, list)) {
            setjump(finfo, list, dtarget);  /* jump to default target */
        } else {
            setjump(finfo, list, vtarget);
        }
        list = next;
    }
}

static void dischargejpc(bfuncinfo *finfo)
{
    patchlistaux(finfo, finfo->jpc, finfo->pc, finfo->pc);
    finfo->jpc = NO_JUMP;
}

static int appendjump(bfuncinfo *finfo, bopcode op, bexpdesc *e)
{
    int j, jpc = finfo->jpc;
    int reg = e ? var2anyreg(finfo, e) : 0;
    finfo->jpc = NO_JUMP;
    if (isK(reg)) {
        reg = be_code_allocregs(finfo, 1);
        code_move(finfo, reg, e->v.idx);
        e->v.idx = reg;
        e->type = ETREG;
    }
    j = codeABx(finfo, op, reg, NO_JUMP + IsBx_MAX);
    be_code_conjump(finfo, &j, jpc);
    return j;
}

int be_code_jump(bfuncinfo *finfo)
{
    return appendjump(finfo, OP_JMP, NULL);
}

void be_code_jumpto(bfuncinfo *finfo, int dst)
{
    be_code_patchlist(finfo, be_code_jump(finfo), dst);
}

void be_code_jumpbool(bfuncinfo *finfo, bexpdesc *e, int jture)
{
    int pc = appendjump(finfo, jumpboolop(e, jture), e);
    be_code_conjump(finfo, jture ? &e->t : &e->f, pc);
    be_code_patchjump(finfo, jture ? e->f : e->t);
    free_expreg(finfo, e);
    jture ? (e->f = NO_JUMP) : (e->t = NO_JUMP);
    e->not = 0;
}

/* connect jump */
void be_code_conjump(bfuncinfo *finfo, int *list, int jmp)
{
    if (jmp == NO_JUMP) {
        return;
    }
    if (*list == NO_JUMP) {
        *list = jmp;
    } else {
        int next, l = *list;
        while ((next = (get_jump(finfo, l))) != NO_JUMP) {
            l = next;
        }
        setjump(finfo, l, jmp);
    }
}

void be_code_patchlist(bfuncinfo *finfo, int list, int dst)
{
    if (dst == finfo->pc) {
        be_code_patchjump(finfo, list);
    } else {
        patchlistaux(finfo, list, dst, dst);
    }
}

void be_code_patchjump(bfuncinfo *finfo, int jmp)
{
    be_code_conjump(finfo, &finfo->jpc, jmp);
    dischargejpc(finfo);  /* 'pc' will change */
}

static int newconst(bfuncinfo *finfo, bvalue *k)
{
    int idx = be_vector_count(&finfo->kvec);
    be_vector_append(&finfo->kvec, k);
    finfo->proto->ktab = be_vector_data(&finfo->kvec);
    finfo->proto->nconst = be_vector_count(&finfo->kvec);
    if (k == NULL) {
        var_setnil(&finfo->proto->ktab[idx]);
    }
    return idx;
}

static int findconst(bfuncinfo *finfo, bexpdesc *e)
{
    int i, count = be_vector_count(&finfo->kvec);
    /* if the constant table is too large, the lookup
     * operation will become very time consuming.
     * so only search the constant table for the
     * previous value.
     **/
    count = count < 50 ? count : 50;
    for (i = 0; i < count; ++i) {
        bvalue *k = be_vector_at(&finfo->kvec, i);
        switch (e->type) {
        case ETINT:
            if (var_isint(k) && k->v.i == e->v.i) {
                return i;
            }
            break;
        case ETREAL:
            if (var_isreal(k) && k->v.r == e->v.r) {
                return i;
            }
            break;
        case ETSTRING:
            if (var_isstr(k) && be_eqstr(k->v.p, e->v.s)) {
                return i;
            }
            break;
        default:
            break;
        }
    }
    return -1;
}

static int exp2const(bfuncinfo *finfo, bexpdesc *e)
{
    int idx = findconst(finfo, e);
    if (idx == -1) {
        bvalue k;
        switch (e->type) {
        case ETINT:
            k.type = BE_INT;
            k.v.i = e->v.i;
            break;
        case ETREAL:
            k.type = BE_REAL;
            k.v.r = e->v.r;
            break;
        case ETSTRING:
            k.type = BE_STRING;
            k.v.s = e->v.s;
            break;
        default:
            break;
        }
        idx = newconst(finfo, &k);
    }
    if (idx < 256) {
        e->type = ETCONST;
        e->v.idx = setK(idx);
    } else { /* index value is too large */
        e->type = ETREG;
        e->v.idx = be_code_allocregs(finfo, 1);
        codeABx(finfo, OP_LDCONST, e->v.idx, idx);
    }
    return e->v.idx;
}

static void free_suffix(bfuncinfo *finfo, bexpdesc *e)
{
    int idx = e->v.ss.idx;
    int nlocal = be_list_count(finfo->local);
    if (!isK(idx) && idx > nlocal) {
        be_code_freeregs(finfo, 1);
    }
    if (e->v.ss.tt == ETREG && (int)e->v.ss.obj > nlocal) {
        be_code_freeregs(finfo, 1);
    }
}

static int code_suffix(bfuncinfo *finfo, bopcode op, bexpdesc *e, int dst)
{
    free_suffix(finfo, e); /* free temporary register */
    if (dst > finfo->freereg) {
        dst = finfo->freereg;
    }
    codeABC(finfo, op, dst, e->v.ss.obj, e->v.ss.idx);
    return dst;
}

static void code_closure(bfuncinfo *finfo, bproto *proto, int dst)
{
    int idx = be_vector_count(&finfo->pvec);
    /* append proto to current function proto table */
    be_vector_append(&finfo->pvec, &proto);
    finfo->proto->ptab = be_vector_data(&finfo->pvec);
    finfo->proto->nproto = be_vector_count(&finfo->pvec);
    codeABx(finfo, OP_CLOSURE, dst, idx); /* load closure to register */
}

static int var2reg(bfuncinfo *finfo, bexpdesc *e, int dst)
{
    switch (e->type) {
    case ETINT:
        if (e->v.i >= IsBx_MIN && e->v.i <= IsBx_MAX) {
            codeABx(finfo, OP_LDINT, dst, var_toidx(e) + IsBx_MAX);
        } else {
            return exp2const(finfo, e);
        }
        break;
    case ETBOOL:
        code_bool(finfo, dst, e->v.i != 0, 0);
        break;
    case ETNIL:
        codeABx(finfo, OP_LDNIL, dst, 0);
        break;
    case ETREAL:
    case ETSTRING:
        return exp2const(finfo, e);
    case ETPROTO:
        code_closure(finfo, e->v.p, dst);
        break;
    case ETGLOBAL:
        codeABx(finfo, OP_GETGBL, dst, e->v.idx);
        break;
    case ETUPVAL:
        codeABx(finfo, OP_GETUPV, dst, e->v.idx);
        break;
    case ETMEMBER:
        dst = code_suffix(finfo, OP_GETMBR, e, dst);
        break;
    case ETINDEX:
        dst = code_suffix(finfo, OP_GETIDX, e, dst);
        break;
    case ETLOCAL: case ETREG: case ETCONST:
        return e->v.idx;
    default:
        return dst; /* error */
    }
    if (dst == finfo->freereg) { /* use a new register */
        be_code_allocregs(finfo, 1);
    }
    e->type = ETREG;
    e->v.idx = dst;
    return dst;
}

static int exp2reg(bfuncinfo *finfo, bexpdesc *e, int dst)
{
    int reg = var2reg(finfo, e, dst);
    if (hasjump(e)) {
        int pcf = NO_JUMP;  /* position of an eventual LOAD false */
        int pct = NO_JUMP;  /* position of an eventual LOAD true */
        int jpt = appendjump(finfo, jumpboolop(e, 1), e);
        reg = e->v.idx;
        be_code_conjump(finfo, &e->t, jpt);
        pcf = code_bool(finfo, reg, 0, 1);
        pct = code_bool(finfo, reg, 1, 0);
        patchlistaux(finfo, e->f, finfo->pc, pcf);
        patchlistaux(finfo, e->t, finfo->pc, pct);
        e->t = NO_JUMP;
        e->f = NO_JUMP;
        e->not = 0;
    }
    return reg;
}

static int codedestreg(bfuncinfo *finfo, bexpdesc *e1, bexpdesc *e2)
{
    int dst, con1 = e1->type == ETREG, con2 = e2->type == ETREG;

    if (con1 && con2) {
        dst = min(e1->v.idx, e2->v.idx);
        be_code_freeregs(finfo, 1);
    } else if (con1) {
        dst = e1->v.idx;
    } else if (con2) {
        dst = e2->v.idx;
    } else {
        dst = be_code_allocregs(finfo, 1);
    }
    return dst;
}

static void binryexp(bfuncinfo *finfo, bopcode op, bexpdesc *e1, bexpdesc *e2)
{
    int src1 = exp2anyreg(finfo, e1);
    int src2 = exp2anyreg(finfo, e2);
    int dst = codedestreg(finfo, e1, e2);
    codeABC(finfo, op, dst, src1, src2);
    e1->type = ETREG;
    e1->v.idx = dst;
}

void be_code_prebinop(bfuncinfo *finfo, int op, bexpdesc *e)
{
    switch (op) {
    case OptAnd:
        be_code_jumpbool(finfo, e, bfalse);
        break;
    case OptOr:
        be_code_jumpbool(finfo, e, btrue);
        break;
    default:
        break;
    }
}

void be_code_binop(bfuncinfo *finfo, int op, bexpdesc *e1, bexpdesc *e2)
{
    switch (op) {
    case OptAnd:
        var2anyreg(finfo, e2);
        be_code_conjump(finfo, &e2->f, e1->f);
        *e1 = *e2;
        break;
    case OptOr:
        var2anyreg(finfo, e2);
        be_code_conjump(finfo, &e2->t, e1->t);
        *e1 = *e2;
        break;
    case OptAdd: case OptSub: case OptMul: case OptDiv:
    case OptMod: case OptLT: case OptLE: case OptEQ:
    case OptNE: case OptGT: case OptGE: case OptRange:
    case OptBitAnd: case OptBitOr: case OptBitXor:
    case OptShiftL: case OptShiftR:
        binryexp(finfo, (bopcode)(op - OptAdd), e1, e2);
        break;
    default: break;
    }
}

static int code_neg(bfuncinfo *finfo, bexpdesc *e)
{
    switch (e->type) {
    case ETINT: e->v.i = -e->v.i; break;
    case ETREAL: e->v.r = -e->v.r; break;
    case ETNIL: case ETBOOL: case ETSTRING:
        return 1; /* error */
    default: {
        int src = exp2anyreg(finfo, e);
        int dst = e->type == ETREG ? src : be_code_allocregs(finfo, 1);
        codeABC(finfo, OP_NEG, dst, src, 0);
        return 0;
    }
    }
    exp2anyreg(finfo, e);
    return 0;
}

static int code_flip(bfuncinfo *finfo, bexpdesc *e)
{
    switch (e->type) {
    case ETINT: e->v.i = ~e->v.i; break;
    case ETREAL: case ETNIL: case ETBOOL: case ETSTRING:
        return 1; /* error */
    default: {
        int src = exp2anyreg(finfo, e);
        int dst = e->type == ETREG ? src : be_code_allocregs(finfo, 1);
        codeABC(finfo, OP_NOT, dst, src, 0);
        return 0;
    }
    }
    exp2anyreg(finfo, e);
    return 0;
}

int be_code_unop(bfuncinfo *finfo, int op, bexpdesc *e)
{
    switch (op) {
    case OptNot: {
        int temp = e->t;
        e->t = e->f;
        e->f = temp;
        e->not = !e->not;
        break;
    }
    case OptFlip: /* do nothing */
        return code_flip(finfo, e);
    case OptSub:
        return code_neg(finfo, e);
    default:
        break;
    }
    return 0;
}

static void setsupvar(bfuncinfo *finfo, bopcode op, bexpdesc *e1, bexpdesc *e2)
{
    int src = exp2anyreg(finfo, e2);
    free_expreg(finfo, e2);
    if (isK(src)) { /* move const to register */
        code_move(finfo, finfo->freereg, src);
        src = finfo->freereg;
    }
    codeABx(finfo, op, src, e1->v.idx);
}

static void setsfxvar(bfuncinfo *finfo, bopcode op, bexpdesc *e1, bexpdesc *e2)
{
    int obj = e1->v.ss.obj;
    int src = exp2anyreg(finfo, e2);
    free_expreg(finfo, e2);
    free_suffix(finfo, e1);
    if (isK(obj)) { /* move const to register */
        code_move(finfo, finfo->freereg, obj);
        obj = finfo->freereg;
    }
    codeABC(finfo, op, obj, e1->v.ss.idx, src);
}

int be_code_setvar(bfuncinfo *finfo, bexpdesc *e1, bexpdesc *e2)
{
    switch (e1->type) {
    case ETLOCAL: { /* It can't be ETREG. */
        int src = exp2reg(finfo, e2, e1->v.idx);
        if (e1->v.idx != src) {
            code_move(finfo, e1->v.idx, src);
        }
        break;
    }
    case ETGLOBAL: /* store to grobal R(A) -> G(Bx) */
        setsupvar(finfo, OP_SETGBL, e1, e2);
        break;
    case ETUPVAL:
        setsupvar(finfo, OP_SETUPV, e1, e2);
        break;
    case ETMEMBER: /* store to member R(A).RK(B) <- RK(C) */
        setsfxvar(finfo, OP_SETMBR, e1, e2);
        break;
    case ETINDEX: /* store to member R(A)[RK(B)] <- RK(C) */
        setsfxvar(finfo, OP_SETIDX, e1, e2);
        break;
    default:
        return 1;
    }
    return 0;
}

int be_code_nextreg(bfuncinfo *finfo, bexpdesc *e)
{
    int dst = finfo->freereg;
    int src = exp2anyreg(finfo, e); /* get variable register index */
    if (e->type != ETREG) { /* move local and const to new register */
        code_move(finfo, dst, src);
        be_code_allocregs(finfo, 1);
    } else {
        dst = src;
    }
    return dst;
}

void be_code_getmethod(bfuncinfo *finfo)
{
    binstruction *p = be_vector_end(&finfo->code);
    *p = (*p & IAx_MASK) | ISET_OP(OP_GETMET);
    be_code_allocregs(finfo, 1); /* method [object] args */
}

void be_code_call(bfuncinfo *finfo, int base, int argc)
{
    codeABC(finfo, OP_CALL, base, argc, 0);
    be_code_freeregs(finfo, argc);
}

void be_code_closure(bfuncinfo *finfo, bexpdesc *e, bproto *proto)
{
    int reg = e->type == ETGLOBAL ? finfo->freereg: e->v.idx;
    code_closure(finfo, proto, reg);
    if (e->type == ETGLOBAL) { /* store to grobal R(A) -> G(Bx) */
        codeABx(finfo, OP_SETGBL, reg, e->v.idx);
    }
}

void be_code_close(bfuncinfo *finfo, int isret)
{
    bblockinfo *binfo = finfo->binfo;
    if (isret) { /* in 'return' */
        while (binfo && !binfo->hasupval) {
            binfo = binfo->prev;
        }
        if (binfo) {
            codeABC(finfo, OP_CLOSE, 0, 0, 0);
        }
    } else if (binfo->prev) { /* leave block */
        if (binfo->hasupval) {
            codeABC(finfo, OP_CLOSE, binfo->nactlocals, 0, 0);
        }
    }
}

void be_code_ret(bfuncinfo *finfo, bexpdesc *e)
{
    if (e == NULL) {
        codeABC(finfo, OP_RET, 0, 0, 0);
    } else {
        int reg = exp2anyreg(finfo, e);
        be_code_close(finfo, 1);
        codeABC(finfo, OP_RET, e->type != ETVOID, reg, 0);
        free_expreg(finfo, e);
    }
}

void be_code_member(bfuncinfo *finfo, bexpdesc *c, bexpdesc *k)
{
    c->v.ss.obj = exp2anyreg(finfo, c);
    c->v.ss.idx = exp2anyreg(finfo, k);
    c->v.ss.tt = c->type;
    c->type = ETMEMBER;
}

void be_code_index(bfuncinfo *finfo, bexpdesc *c, bexpdesc *k)
{
    c->v.ss.obj = exp2anyreg(finfo, c);
    c->v.ss.idx = exp2anyreg(finfo, k);
    c->v.ss.tt = c->type;
    c->type = ETINDEX;
}

bvalue* be_code_localobject(bfuncinfo *finfo, int dst)
{
    int src = newconst(finfo, NULL);
    code_move(finfo, dst, setK(src));
    return be_vector_end(&finfo->kvec);
}

void be_code_setsuper(bfuncinfo *finfo, bexpdesc *c, bexpdesc *s)
{
    int self = exp2anyreg(finfo, c);
    int super = exp2anyreg(finfo, s);
    codeABC(finfo, OP_SETSUPER, self, super, 0);
}

void be_code_import(bfuncinfo *finfo, bexpdesc *m, bexpdesc *v)
{
    int dst, src = exp2anyreg(finfo, m);
    if (v->type == ETLOCAL) {
        dst = v->v.idx;
        codeABC(finfo, OP_IMPORT, dst, src, 0);
    } else {
        dst = be_code_allocregs(finfo, 1);
        codeABC(finfo, OP_IMPORT, dst, src, 0);
        m->type = ETREG;
        m->v.idx = dst;
        be_code_setvar(finfo, v, m);
    }
}
