use crate::error::Span;

#[derive(Debug, Clone)]
pub struct Program {
    pub items: Vec<Item>,
}

#[derive(Debug, Clone)]
pub enum Item {
    FnDecl(FnDecl),
    Use(UseDecl),
}

#[derive(Debug, Clone)]
pub struct UseDecl {
    pub path: Vec<String>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct FnDecl {
    pub name: String,
    pub params: Vec<Param>,
    pub ret_type: Option<TypeExpr>,
    pub body: Block,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub struct Param {
    pub name: String,
    pub ty: TypeExpr,
    pub mutable: bool,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum TypeExpr {
    Named(String, Vec<TypeExpr>),   // e.g. Vec<i64>
    Ref(Box<TypeExpr>, bool),       // &T or &mut T
    Unit,
}

#[derive(Debug, Clone)]
pub struct Block {
    pub stmts: Vec<Stmt>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum Stmt {
    Let(LetStmt),
    Expr(Expr),
    Semi(Expr),
    Return(Option<Expr>, Span),
    Break(Span),
    Continue(Span),
}

#[derive(Debug, Clone)]
pub struct LetStmt {
    pub name: String,
    pub mutable: bool,
    pub ty: Option<TypeExpr>,
    pub init: Option<Expr>,
    pub span: Span,
}

#[derive(Debug, Clone)]
pub enum Expr {
    Lit(Lit, Span),
    Ident(String, Span),
    Assign(Box<Expr>, Box<Expr>, Span),
    CompoundAssign(BinOp, Box<Expr>, Box<Expr>, Span),
    BinOp(BinOp, Box<Expr>, Box<Expr>, Span),
    UnOp(UnOp, Box<Expr>, Span),
    Call(Box<Expr>, Vec<Expr>, Span),
    MacroCall(String, Vec<Expr>, Span),
    FieldAccess(Box<Expr>, String, Span),
    Index(Box<Expr>, Box<Expr>, Span),
    MethodCall(Box<Expr>, String, Vec<Expr>, Span),
    If(Box<Expr>, Block, Option<Box<IfElse>>, Span),
    While(Box<Expr>, Block, Span),
    Loop(Block, Span),
    For(String, Box<Expr>, Block, Span),
    Block(Block),
    Range(Option<Box<Expr>>, Option<Box<Expr>>, bool, Span), // start..end or start..=end
    Array(Vec<Expr>, Span),
    Tuple(Vec<Expr>, Span),
    Return(Box<Expr>, Span),
    Break(Span),
    Continue(Span),
}

#[derive(Debug, Clone)]
pub enum IfElse {
    ElseIf(Expr),
    Else(Block),
}

impl Expr {
    pub fn span(&self) -> Span {
        match self {
            Expr::Lit(_, s) => s.clone(),
            Expr::Ident(_, s) => s.clone(),
            Expr::Assign(_, _, s) => s.clone(),
            Expr::CompoundAssign(_, _, _, s) => s.clone(),
            Expr::BinOp(_, _, _, s) => s.clone(),
            Expr::UnOp(_, _, s) => s.clone(),
            Expr::Call(_, _, s) => s.clone(),
            Expr::MacroCall(_, _, s) => s.clone(),
            Expr::FieldAccess(_, _, s) => s.clone(),
            Expr::Index(_, _, s) => s.clone(),
            Expr::MethodCall(_, _, _, s) => s.clone(),
            Expr::If(_, _, _, s) => s.clone(),
            Expr::While(_, _, s) => s.clone(),
            Expr::Loop(_, s) => s.clone(),
            Expr::For(_, _, _, s) => s.clone(),
            Expr::Block(b) => b.span.clone(),
            Expr::Range(_, _, _, s) => s.clone(),
            Expr::Array(_, s) => s.clone(),
            Expr::Tuple(_, s) => s.clone(),
            Expr::Return(_, s) => s.clone(),
            Expr::Break(s) => s.clone(),
            Expr::Continue(s) => s.clone(),
        }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum BinOp {
    Add, Sub, Mul, Div, Rem,
    Eq, Ne, Lt, Le, Gt, Ge,
    And, Or,
    BitAnd, BitOr, BitXor, Shl, Shr,
}

#[derive(Debug, Clone, PartialEq)]
pub enum UnOp {
    Neg, Not, Ref, RefMut, Deref,
}

#[derive(Debug, Clone)]
pub enum Lit {
    Int(i64),
    Float(f64),
    Str(String),
    Bool(bool),
    Char(char),
    Unit,
}
