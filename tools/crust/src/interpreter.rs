use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::fmt;

use crate::ast::*;
use crate::environment::{Env, ValueRef};
use crate::error::{CrustError, CrustResult, Span};

#[derive(Clone, Debug)]
pub enum Value {
    Int(i64),
    Float(f64),
    Bool(bool),
    Str(String),
    Char(char),
    Unit,
    Vec(Vec<ValueRef>),
    Tuple(Vec<ValueRef>),
    Function(FnValue),
    Builtin(String),
}

#[derive(Clone, Debug)]
pub struct FnValue {
    pub name: String,
    pub params: Vec<Param>,
    pub body: Block,
    pub closure: Option<Env>,
}

impl fmt::Display for Value {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Value::Int(n)   => write!(f, "{}", n),
            Value::Float(n) => {
                if n.fract() == 0.0 {
                    write!(f, "{:.1}", n)
                } else {
                    write!(f, "{}", n)
                }
            }
            Value::Bool(b)  => write!(f, "{}", b),
            Value::Str(s)   => write!(f, "{}", s),
            Value::Char(c)  => write!(f, "{}", c),
            Value::Unit     => write!(f, "()"),
            Value::Vec(v)   => {
                write!(f, "[")?;
                for (i, item) in v.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{}", item.borrow())?;
                }
                write!(f, "]")
            }
            Value::Tuple(t) => {
                write!(f, "(")?;
                for (i, item) in t.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{}", item.borrow())?;
                }
                write!(f, ")")
            }
            Value::Function(fv) => write!(f, "<fn {}>", fv.name),
            Value::Builtin(name) => write!(f, "<builtin {}>", name),
        }
    }
}

fn wrap(v: Value) -> ValueRef {
    Rc::new(RefCell::new(v))
}

fn runtime_err(msg: impl Into<String>, span: Option<Span>) -> CrustError {
    CrustError::RuntimeError { msg: msg.into(), span }
}

pub struct Interpreter {
    env: Env,
    functions: HashMap<String, FnValue>,
}

impl Interpreter {
    pub fn new() -> Self {
        Interpreter {
            env: Env::new(),
            functions: HashMap::new(),
        }
    }

    pub fn run_program(&mut self, program: &Program) -> CrustResult<()> {
        // First pass: register all functions
        for item in &program.items {
            if let Item::FnDecl(f) = item {
                self.functions.insert(f.name.clone(), FnValue {
                    name: f.name.clone(),
                    params: f.params.clone(),
                    body: f.body.clone(),
                    closure: None,
                });
            }
        }
        // Execute main
        let main_fn = self.functions.get("main").cloned().ok_or_else(|| runtime_err("no main function", None))?;
        match self.call_fn(&main_fn, vec![], None) {
            Ok(_) => Ok(()),
            Err(CrustError::Return(_)) => Ok(()),
            Err(e) => Err(e),
        }
    }

    fn call_fn(&mut self, func: &FnValue, args: Vec<ValueRef>, span: Option<Span>) -> CrustResult<ValueRef> {
        if func.params.len() != args.len() {
            return Err(runtime_err(
                format!("function '{}' expects {} args, got {}", func.name, func.params.len(), args.len()),
                span,
            ));
        }
        self.env.push_scope();
        for (param, arg) in func.params.iter().zip(args.iter()) {
            self.env.define(&param.name, Rc::clone(arg));
        }
        let result = self.exec_block(&func.body);
        self.env.pop_scope();
        match result {
            Ok(v) => Ok(v),
            Err(CrustError::Return(v)) => Ok(wrap(v)),
            Err(e) => Err(e),
        }
    }

    fn exec_block(&mut self, block: &Block) -> CrustResult<ValueRef> {
        let mut last = wrap(Value::Unit);
        for stmt in &block.stmts {
            last = self.exec_stmt(stmt)?;
        }
        Ok(last)
    }

    fn exec_stmt(&mut self, stmt: &Stmt) -> CrustResult<ValueRef> {
        match stmt {
            Stmt::Let(let_stmt) => {
                let val = if let Some(init) = &let_stmt.init {
                    self.eval_expr(init)?
                } else {
                    wrap(Value::Unit)
                };
                // In hack mode, always clone the Rc
                self.env.define(&let_stmt.name, Rc::clone(&val));
                Ok(wrap(Value::Unit))
            }
            Stmt::Expr(expr) => self.eval_expr(expr),
            Stmt::Semi(expr) => {
                self.eval_expr(expr)?;
                Ok(wrap(Value::Unit))
            }
            Stmt::Return(Some(expr), _) => {
                let v = self.eval_expr(expr)?;
                let inner = v.borrow().clone();
                Err(CrustError::Return(inner))
            }
            Stmt::Return(None, _) => {
                Err(CrustError::Return(Value::Unit))
            }
            Stmt::Break(_) => Err(CrustError::Break),
            Stmt::Continue(_) => Err(CrustError::Continue),
        }
    }

    pub fn eval_expr(&mut self, expr: &Expr) -> CrustResult<ValueRef> {
        match expr {
            Expr::Lit(lit, _) => Ok(wrap(self.eval_lit(lit))),

            Expr::Ident(name, span) => {
                // Check builtins first
                match name.as_str() {
                    "String" | "Vec" | "HashMap" => return Ok(wrap(Value::Builtin(name.clone()))),
                    _ => {}
                }
                // Check env, fall back to function table
                match self.env.get(name, span) {
                    Ok(v) => Ok(v),
                    Err(_) => {
                        if let Some(f) = self.functions.get(name).cloned() {
                            Ok(wrap(Value::Function(f)))
                        } else {
                            Err(CrustError::RuntimeError {
                                msg: format!("undefined variable '{}'", name),
                                span: Some(span.clone()),
                            })
                        }
                    }
                }
            }

            Expr::Assign(lhs, rhs, span) => {
                let val = self.eval_expr(rhs)?;
                self.do_assign(lhs, val, span)?;
                Ok(wrap(Value::Unit))
            }

            Expr::CompoundAssign(op, lhs, rhs, span) => {
                let lval = self.eval_expr(lhs)?;
                let rval = self.eval_expr(rhs)?;
                let result = self.apply_binop(op, &lval.borrow(), &rval.borrow(), span)?;
                self.do_assign(lhs, wrap(result), span)?;
                Ok(wrap(Value::Unit))
            }

            Expr::BinOp(op, l, r, span) => {
                let lv = self.eval_expr(l)?;
                let rv = self.eval_expr(r)?;
                let result = self.apply_binop(op, &lv.borrow(), &rv.borrow(), span)?;
                Ok(wrap(result))
            }

            Expr::UnOp(op, e, span) => {
                let v = self.eval_expr(e)?;
                let result = self.apply_unop(op, &v.borrow(), span)?;
                Ok(wrap(result))
            }

            Expr::MacroCall(name, args, span) => {
                self.eval_macro(name, args, span)
            }

            Expr::Call(callee, args, span) => {
                // Evaluate callee
                let callee_val = self.eval_expr(callee)?;
                let mut evaled_args = Vec::new();
                for a in args {
                    evaled_args.push(self.eval_expr(a)?);
                }
                self.call_value(callee_val, evaled_args, span)
            }

            Expr::MethodCall(obj, method, args, span) => {
                let obj_val = self.eval_expr(obj)?;
                let mut evaled_args = Vec::new();
                for a in args {
                    evaled_args.push(self.eval_expr(a)?);
                }
                self.call_method(obj_val, method, evaled_args, span)
            }

            Expr::FieldAccess(obj, field, span) => {
                let obj_val = self.eval_expr(obj)?;
                // Handle Type::method style (FieldAccess of Builtin)
                match &*obj_val.borrow() {
                    Value::Builtin(type_name) => {
                        return Ok(wrap(Value::Builtin(format!("{}::{}", type_name, field))));
                    }
                    Value::Tuple(items) => {
                        if let Ok(idx) = field.parse::<usize>() {
                            return items.get(idx)
                                .map(|v| Rc::clone(v))
                                .ok_or_else(|| runtime_err(format!("tuple index {} out of range", idx), Some(span.clone())));
                        }
                    }
                    _ => {}
                }
                Err(runtime_err(format!("no field '{}' on value", field), Some(span.clone())))
            }

            Expr::Index(obj, idx, span) => {
                let obj_val = self.eval_expr(obj)?;
                let idx_val = self.eval_expr(idx)?;
                let obj_inner = obj_val.borrow().clone();
                let idx_inner = idx_val.borrow().clone();
                match (&obj_inner, &idx_inner) {
                    (Value::Vec(items), Value::Int(i)) => {
                        let i = *i as usize;
                        items.get(i).map(|v| Rc::clone(v)).ok_or_else(|| runtime_err(
                            format!("index {} out of bounds (len={})", i, items.len()), Some(span.clone())
                        ))
                    }
                    _ => Err(runtime_err("invalid index operation", Some(span.clone()))),
                }
            }

            Expr::If(cond, then_block, else_branch, _) => {
                let cv = self.eval_expr(cond)?;
                if self.is_truthy(&cv.borrow()) {
                    self.env.push_scope();
                    let r = self.exec_block(then_block);
                    self.env.pop_scope();
                    r
                } else if let Some(else_b) = else_branch {
                    match else_b.as_ref() {
                        IfElse::ElseIf(elif_expr) => self.eval_expr(elif_expr),
                        IfElse::Else(else_block) => {
                            self.env.push_scope();
                            let r = self.exec_block(else_block);
                            self.env.pop_scope();
                            r
                        }
                    }
                } else {
                    Ok(wrap(Value::Unit))
                }
            }

            Expr::While(cond, body, _) => {
                loop {
                    let cv = self.eval_expr(cond)?;
                    if !self.is_truthy(&cv.borrow()) { break; }
                    self.env.push_scope();
                    let result = self.exec_block(body);
                    self.env.pop_scope();
                    match result {
                        Ok(_) => {}
                        Err(CrustError::Break) => break,
                        Err(CrustError::Continue) => continue,
                        Err(e) => return Err(e),
                    }
                }
                Ok(wrap(Value::Unit))
            }

            Expr::Loop(body, _) => {
                loop {
                    self.env.push_scope();
                    let result = self.exec_block(body);
                    self.env.pop_scope();
                    match result {
                        Ok(_) => {}
                        Err(CrustError::Break) => break,
                        Err(CrustError::Continue) => continue,
                        Err(e) => return Err(e),
                    }
                }
                Ok(wrap(Value::Unit))
            }

            Expr::For(var, iter_expr, body, span) => {
                let iter_val = self.eval_expr(iter_expr)?;
                let items = self.to_iter(iter_val, span)?;
                for item in items {
                    self.env.push_scope();
                    self.env.define(var, item);
                    let result = self.exec_block(body);
                    self.env.pop_scope();
                    match result {
                        Ok(_) => {}
                        Err(CrustError::Break) => break,
                        Err(CrustError::Continue) => continue,
                        Err(e) => return Err(e),
                    }
                }
                Ok(wrap(Value::Unit))
            }

            Expr::Block(block) => {
                self.env.push_scope();
                let r = self.exec_block(block);
                self.env.pop_scope();
                r
            }

            Expr::Range(start, end, inclusive, span) => {
                let start_val = if let Some(s) = start {
                    match &*self.eval_expr(s)?.borrow() {
                        Value::Int(n) => *n,
                        _ => return Err(runtime_err("range start must be integer", Some(span.clone()))),
                    }
                } else { 0 };
                let end_val = if let Some(e) = end {
                    match &*self.eval_expr(e)?.borrow() {
                        Value::Int(n) => *n,
                        _ => return Err(runtime_err("range end must be integer", Some(span.clone()))),
                    }
                } else {
                    return Err(runtime_err("open-ended range not supported as iterator", Some(span.clone())));
                };
                let range: Vec<ValueRef> = if *inclusive {
                    (start_val..=end_val).map(|n| wrap(Value::Int(n))).collect()
                } else {
                    (start_val..end_val).map(|n| wrap(Value::Int(n))).collect()
                };
                Ok(wrap(Value::Vec(range)))
            }

            Expr::Array(elems, _) => {
                let mut items = Vec::new();
                for e in elems {
                    items.push(self.eval_expr(e)?);
                }
                Ok(wrap(Value::Vec(items)))
            }

            Expr::Tuple(elems, _) => {
                let mut items = Vec::new();
                for e in elems {
                    items.push(self.eval_expr(e)?);
                }
                Ok(wrap(Value::Tuple(items)))
            }

            Expr::Return(e, _) => {
                let v = self.eval_expr(e)?;
                let inner = v.borrow().clone();
                Err(CrustError::Return(inner))
            }

            Expr::Break(_) => Err(CrustError::Break),
            Expr::Continue(_) => Err(CrustError::Continue),
        }
    }

    fn eval_lit(&self, lit: &Lit) -> Value {
        match lit {
            Lit::Int(n)   => Value::Int(*n),
            Lit::Float(f) => Value::Float(*f),
            Lit::Str(s)   => Value::Str(s.clone()),
            Lit::Bool(b)  => Value::Bool(*b),
            Lit::Char(c)  => Value::Char(*c),
            Lit::Unit     => Value::Unit,
        }
    }

    fn is_truthy(&self, v: &Value) -> bool {
        match v {
            Value::Bool(b) => *b,
            Value::Int(n) => *n != 0,
            Value::Unit => false,
            _ => true,
        }
    }

    fn to_iter(&self, val: ValueRef, span: &Span) -> CrustResult<Vec<ValueRef>> {
        match &*val.borrow() {
            Value::Vec(items) => Ok(items.iter().map(|v| Rc::clone(v)).collect()),
            Value::Str(s) => Ok(s.chars().map(|c| wrap(Value::Char(c))).collect()),
            _ => Err(runtime_err("value is not iterable", Some(span.clone()))),
        }
    }

    fn apply_binop(&self, op: &BinOp, l: &Value, r: &Value, span: &Span) -> CrustResult<Value> {
        match (l, r) {
            (Value::Int(a), Value::Int(b)) => {
                let result = match op {
                    BinOp::Add => Value::Int(a + b),
                    BinOp::Sub => Value::Int(a - b),
                    BinOp::Mul => Value::Int(a * b),
                    BinOp::Div => {
                        if *b == 0 { return Err(runtime_err("division by zero", Some(span.clone()))); }
                        Value::Int(a / b)
                    }
                    BinOp::Rem => {
                        if *b == 0 { return Err(runtime_err("remainder by zero", Some(span.clone()))); }
                        Value::Int(a % b)
                    }
                    BinOp::Eq  => Value::Bool(a == b),
                    BinOp::Ne  => Value::Bool(a != b),
                    BinOp::Lt  => Value::Bool(a < b),
                    BinOp::Le  => Value::Bool(a <= b),
                    BinOp::Gt  => Value::Bool(a > b),
                    BinOp::Ge  => Value::Bool(a >= b),
                    BinOp::And => Value::Bool(*a != 0 && *b != 0),
                    BinOp::Or  => Value::Bool(*a != 0 || *b != 0),
                    BinOp::BitAnd => Value::Int(a & b),
                    BinOp::BitOr  => Value::Int(a | b),
                    BinOp::BitXor => Value::Int(a ^ b),
                    BinOp::Shl    => Value::Int(a << b),
                    BinOp::Shr    => Value::Int(a >> b),
                };
                Ok(result)
            }
            (Value::Float(a), Value::Float(b)) => {
                Ok(match op {
                    BinOp::Add => Value::Float(a + b),
                    BinOp::Sub => Value::Float(a - b),
                    BinOp::Mul => Value::Float(a * b),
                    BinOp::Div => Value::Float(a / b),
                    BinOp::Rem => Value::Float(a % b),
                    BinOp::Eq  => Value::Bool(a == b),
                    BinOp::Ne  => Value::Bool(a != b),
                    BinOp::Lt  => Value::Bool(a < b),
                    BinOp::Le  => Value::Bool(a <= b),
                    BinOp::Gt  => Value::Bool(a > b),
                    BinOp::Ge  => Value::Bool(a >= b),
                    BinOp::And => Value::Bool(*a != 0.0 && *b != 0.0),
                    BinOp::Or  => Value::Bool(*a != 0.0 || *b != 0.0),
                    _ => return Err(runtime_err("unsupported float operation", Some(span.clone()))),
                })
            }
            (Value::Int(a), Value::Float(b)) => {
                self.apply_binop(op, &Value::Float(*a as f64), &Value::Float(*b), span)
            }
            (Value::Float(a), Value::Int(b)) => {
                self.apply_binop(op, &Value::Float(*a), &Value::Float(*b as f64), span)
            }
            (Value::Bool(a), Value::Bool(b)) => {
                Ok(match op {
                    BinOp::And | BinOp::BitAnd => Value::Bool(*a && *b),
                    BinOp::Or  | BinOp::BitOr  => Value::Bool(*a || *b),
                    BinOp::Eq  => Value::Bool(a == b),
                    BinOp::Ne  => Value::Bool(a != b),
                    _ => return Err(runtime_err("unsupported bool operation", Some(span.clone()))),
                })
            }
            (Value::Str(a), Value::Str(b)) => {
                Ok(match op {
                    BinOp::Add => Value::Str(format!("{}{}", a, b)),
                    BinOp::Eq  => Value::Bool(a == b),
                    BinOp::Ne  => Value::Bool(a != b),
                    BinOp::Lt  => Value::Bool(a < b),
                    BinOp::Le  => Value::Bool(a <= b),
                    BinOp::Gt  => Value::Bool(a > b),
                    BinOp::Ge  => Value::Bool(a >= b),
                    _ => return Err(runtime_err("unsupported string operation", Some(span.clone()))),
                })
            }
            (Value::Str(a), _) => {
                if let BinOp::Add = op {
                    Ok(Value::Str(format!("{}{}", a, r)))
                } else {
                    Err(runtime_err(format!("cannot apply {:?} to string and {:?}", op, r), Some(span.clone())))
                }
            }
            _ => Err(runtime_err(
                format!("cannot apply {:?} to {:?} and {:?}", op, l, r),
                Some(span.clone()),
            )),
        }
    }

    fn apply_unop(&self, op: &UnOp, v: &Value, span: &Span) -> CrustResult<Value> {
        match op {
            UnOp::Neg => match v {
                Value::Int(n) => Ok(Value::Int(-n)),
                Value::Float(f) => Ok(Value::Float(-f)),
                _ => Err(runtime_err("cannot negate non-numeric", Some(span.clone()))),
            },
            UnOp::Not => match v {
                Value::Bool(b) => Ok(Value::Bool(!b)),
                Value::Int(n) => Ok(Value::Int(!n)),
                _ => Err(runtime_err("cannot apply ! to value", Some(span.clone()))),
            },
            UnOp::Deref | UnOp::Ref | UnOp::RefMut => {
                // In hack mode, refs are transparent
                Ok(v.clone())
            }
        }
    }

    fn do_assign(&mut self, lhs: &Expr, val: ValueRef, span: &Span) -> CrustResult<()> {
        match lhs {
            Expr::Ident(name, s) => {
                self.env.set(name, val, s)?;
                Ok(())
            }
            Expr::Index(obj, idx, s) => {
                let idx_val = self.eval_expr(idx)?;
                let obj_ref = self.eval_expr(obj)?;
                let mut obj_borrow = obj_ref.borrow_mut();
                match &mut *obj_borrow {
                    Value::Vec(items) => {
                        let i = match &*idx_val.borrow() {
                            Value::Int(n) => *n as usize,
                            _ => return Err(runtime_err("index must be integer", Some(s.clone()))),
                        };
                        if i < items.len() {
                            items[i] = val;
                            Ok(())
                        } else {
                            Err(runtime_err(format!("index {} out of bounds", i), Some(s.clone())))
                        }
                    }
                    _ => Err(runtime_err("cannot index non-vec", Some(s.clone()))),
                }
            }
            Expr::FieldAccess(_, field, s) => {
                Err(runtime_err(format!("cannot assign to field '{}' in hack mode (structs not yet supported)", field), Some(s.clone())))
            }
            _ => Err(runtime_err("invalid assignment target", Some(span.clone()))),
        }
    }

    fn call_value(&mut self, callee: ValueRef, args: Vec<ValueRef>, span: &Span) -> CrustResult<ValueRef> {
        let func = match callee.borrow().clone() {
            Value::Function(f) => f,
            Value::Builtin(name) => return self.call_builtin(&name, args, span),
            other => return Err(runtime_err(
                format!("cannot call non-function: {}", other), Some(span.clone())
            )),
        };
        // Look up fresh from function table (functions can be recursive)
        let func = self.functions.get(&func.name).cloned().unwrap_or(func);
        self.call_fn(&func, args, Some(span.clone()))
    }

    fn call_builtin(&mut self, name: &str, args: Vec<ValueRef>, span: &Span) -> CrustResult<ValueRef> {
        match name {
            "Vec::new" | "Vec" => Ok(wrap(Value::Vec(vec![]))),
            "String::new" | "String" => Ok(wrap(Value::Str(String::new()))),
            "String::from" => {
                let s = args.get(0)
                    .map(|v| format!("{}", v.borrow()))
                    .unwrap_or_default();
                Ok(wrap(Value::Str(s)))
            }
            _ => Err(runtime_err(format!("unknown builtin '{}'", name), Some(span.clone()))),
        }
    }

    fn call_method(&mut self, obj: ValueRef, method: &str, args: Vec<ValueRef>, span: &Span) -> CrustResult<ValueRef> {
        // Clone inner value to avoid borrow conflicts
        let inner = obj.borrow().clone();
        match (&inner, method) {
            // Vec methods
            (Value::Vec(_), "push") => {
                let arg = args.into_iter().next().ok_or_else(|| runtime_err("push expects 1 arg", Some(span.clone())))?;
                if let Value::Vec(ref mut v) = *obj.borrow_mut() {
                    v.push(arg);
                }
                Ok(wrap(Value::Unit))
            }
            (Value::Vec(_), "pop") => {
                let popped = if let Value::Vec(ref mut v) = *obj.borrow_mut() {
                    v.pop().map(|item| item.borrow().clone())
                } else { None };
                Ok(wrap(popped.unwrap_or(Value::Unit)))
            }
            (Value::Vec(v), "len") => Ok(wrap(Value::Int(v.len() as i64))),
            (Value::Vec(v), "is_empty") => Ok(wrap(Value::Bool(v.is_empty()))),
            (Value::Vec(v), "get") => {
                let idx = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Int(n)) => n as usize,
                    _ => return Err(runtime_err("get expects integer index", Some(span.clone()))),
                };
                Ok(wrap(v.get(idx).map(|item| item.borrow().clone()).unwrap_or(Value::Unit)))
            }
            (Value::Vec(_), "sort") => {
                if let Value::Vec(ref mut v) = *obj.borrow_mut() {
                    v.sort_by(|a, b| {
                        match (&*a.borrow(), &*b.borrow()) {
                            (Value::Int(x), Value::Int(y)) => x.cmp(y),
                            (Value::Float(x), Value::Float(y)) => x.partial_cmp(y).unwrap_or(std::cmp::Ordering::Equal),
                            (Value::Str(x), Value::Str(y)) => x.cmp(y),
                            _ => std::cmp::Ordering::Equal,
                        }
                    });
                }
                Ok(wrap(Value::Unit))
            }
            (Value::Vec(v), "contains") => {
                let target = args.get(0).map(|v| v.borrow().clone());
                let found = v.iter().any(|item| {
                    if let Some(ref t) = target {
                        format!("{}", item.borrow()) == format!("{}", t)
                    } else { false }
                });
                Ok(wrap(Value::Bool(found)))
            }
            (Value::Vec(v), "join") => {
                let sep = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Str(s)) => s,
                    _ => "".to_string(),
                };
                let joined: Vec<String> = v.iter().map(|item| format!("{}", item.borrow())).collect();
                Ok(wrap(Value::Str(joined.join(&sep))))
            }
            (Value::Vec(v), "iter") => {
                // In hack mode, iter() returns a clone of the vec
                let cloned: Vec<ValueRef> = v.iter().map(|item| Rc::clone(item)).collect();
                Ok(wrap(Value::Vec(cloned)))
            }
            (Value::Vec(v), "clone") => {
                let cloned: Vec<ValueRef> = v.iter().map(|item| wrap(item.borrow().clone())).collect();
                Ok(wrap(Value::Vec(cloned)))
            }
            (Value::Vec(v), "first") => {
                Ok(v.first().map(|item| wrap(item.borrow().clone())).unwrap_or_else(|| wrap(Value::Unit)))
            }
            (Value::Vec(v), "last") => {
                Ok(v.last().map(|item| wrap(item.borrow().clone())).unwrap_or_else(|| wrap(Value::Unit)))
            }
            (Value::Vec(v), "reverse") => {
                if let Value::Vec(ref mut items) = *obj.borrow_mut() {
                    items.reverse();
                }
                Ok(wrap(Value::Unit))
            }

            // String methods
            (Value::Str(s), "len") => Ok(wrap(Value::Int(s.len() as i64))),
            (Value::Str(s), "is_empty") => Ok(wrap(Value::Bool(s.is_empty()))),
            (Value::Str(s), "to_uppercase") => Ok(wrap(Value::Str(s.to_uppercase()))),
            (Value::Str(s), "to_lowercase") => Ok(wrap(Value::Str(s.to_lowercase()))),
            (Value::Str(s), "trim") => Ok(wrap(Value::Str(s.trim().to_string()))),
            (Value::Str(s), "trim_start") | (Value::Str(s), "trim_left") => Ok(wrap(Value::Str(s.trim_start().to_string()))),
            (Value::Str(s), "trim_end") | (Value::Str(s), "trim_right") => Ok(wrap(Value::Str(s.trim_end().to_string()))),
            (Value::Str(s), "to_string") => Ok(wrap(Value::Str(s.clone()))),
            (Value::Str(s), "clone") => Ok(wrap(Value::Str(s.clone()))),
            (Value::Str(s), "contains") => {
                let pat = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Str(p)) => p,
                    Some(Value::Char(c)) => c.to_string(),
                    _ => return Err(runtime_err("contains expects a string pattern", Some(span.clone()))),
                };
                Ok(wrap(Value::Bool(s.contains(pat.as_str()))))
            }
            (Value::Str(s), "starts_with") => {
                let pat = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Str(p)) => p,
                    _ => return Err(runtime_err("starts_with expects a string", Some(span.clone()))),
                };
                Ok(wrap(Value::Bool(s.starts_with(pat.as_str()))))
            }
            (Value::Str(s), "ends_with") => {
                let pat = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Str(p)) => p,
                    _ => return Err(runtime_err("ends_with expects a string", Some(span.clone()))),
                };
                Ok(wrap(Value::Bool(s.ends_with(pat.as_str()))))
            }
            (Value::Str(s), "replace") => {
                let from = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Str(p)) => p,
                    _ => return Err(runtime_err("replace arg 1 must be string", Some(span.clone()))),
                };
                let to = match args.get(1).map(|v| v.borrow().clone()) {
                    Some(Value::Str(p)) => p,
                    _ => return Err(runtime_err("replace arg 2 must be string", Some(span.clone()))),
                };
                Ok(wrap(Value::Str(s.replace(from.as_str(), to.as_str()))))
            }
            (Value::Str(s), "split") => {
                let sep = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Str(p)) => p,
                    Some(Value::Char(c)) => c.to_string(),
                    _ => return Err(runtime_err("split expects a string separator", Some(span.clone()))),
                };
                let parts: Vec<ValueRef> = s.split(sep.as_str()).map(|p| wrap(Value::Str(p.to_string()))).collect();
                Ok(wrap(Value::Vec(parts)))
            }
            (Value::Str(s), "chars") => {
                let chars: Vec<ValueRef> = s.chars().map(|c| wrap(Value::Char(c))).collect();
                Ok(wrap(Value::Vec(chars)))
            }
            (Value::Str(s), "parse") => {
                // Try parse as i64 first, then f64
                if let Ok(n) = s.parse::<i64>() {
                    Ok(wrap(Value::Int(n)))
                } else if let Ok(f) = s.parse::<f64>() {
                    Ok(wrap(Value::Float(f)))
                } else {
                    Err(runtime_err(format!("cannot parse '{}' as number", s), Some(span.clone())))
                }
            }
            (Value::Str(s), "push_str") => {
                let arg = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Str(p)) => p,
                    _ => return Err(runtime_err("push_str expects a string", Some(span.clone()))),
                };
                if let Value::Str(ref mut inner) = *obj.borrow_mut() {
                    inner.push_str(&arg);
                }
                Ok(wrap(Value::Unit))
            }
            (Value::Str(s), "push") => {
                let arg = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Char(c)) => c,
                    _ => return Err(runtime_err("push expects a char", Some(span.clone()))),
                };
                if let Value::Str(ref mut inner) = *obj.borrow_mut() {
                    inner.push(arg);
                }
                Ok(wrap(Value::Unit))
            }
            (Value::Str(s), "lines") => {
                let lines: Vec<ValueRef> = s.lines().map(|l| wrap(Value::Str(l.to_string()))).collect();
                Ok(wrap(Value::Vec(lines)))
            }
            (Value::Str(s), "repeat") => {
                let n = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Int(n)) => n as usize,
                    _ => return Err(runtime_err("repeat expects integer", Some(span.clone()))),
                };
                Ok(wrap(Value::Str(s.repeat(n))))
            }

            // Int/Float methods
            (Value::Int(n), "to_string") => Ok(wrap(Value::Str(n.to_string()))),
            (Value::Float(f), "to_string") => Ok(wrap(Value::Str(f.to_string()))),
            (Value::Int(n), "abs") => Ok(wrap(Value::Int(n.abs()))),
            (Value::Float(f), "abs") => Ok(wrap(Value::Float(f.abs()))),
            (Value::Float(f), "sqrt") => Ok(wrap(Value::Float(f.sqrt()))),
            (Value::Float(f), "floor") => Ok(wrap(Value::Float(f.floor()))),
            (Value::Float(f), "ceil") => Ok(wrap(Value::Float(f.ceil()))),
            (Value::Float(f), "round") => Ok(wrap(Value::Float(f.round()))),
            (Value::Int(n), "pow") => {
                let exp = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Int(e)) => e as u32,
                    _ => return Err(runtime_err("pow expects integer exponent", Some(span.clone()))),
                };
                Ok(wrap(Value::Int(n.pow(exp))))
            }
            (Value::Float(f), "powi") => {
                let exp = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Int(e)) => e as i32,
                    _ => return Err(runtime_err("powi expects integer exponent", Some(span.clone()))),
                };
                Ok(wrap(Value::Float(f.powi(exp))))
            }
            (Value::Float(f), "powf") => {
                let exp = match args.get(0).map(|v| v.borrow().clone()) {
                    Some(Value::Float(e)) => e,
                    Some(Value::Int(e)) => e as f64,
                    _ => return Err(runtime_err("powf expects float exponent", Some(span.clone()))),
                };
                Ok(wrap(Value::Float(f.powf(exp))))
            }

            // Bool methods
            (Value::Bool(b), "to_string") => Ok(wrap(Value::Str(b.to_string()))),

            // Fallthrough: try user-defined method
            _ => {
                // Check if there's a user-defined function with that name
                if let Some(func) = self.functions.get(method).cloned() {
                    let mut all_args = vec![obj];
                    all_args.extend(args);
                    self.call_fn(&func, all_args, Some(span.clone()))
                } else {
                    Err(runtime_err(
                        format!("no method '{}' on {:?}", method, inner),
                        Some(span.clone()),
                    ))
                }
            }
        }
    }

    fn eval_macro(&mut self, name: &str, args: &[Expr], span: &Span) -> CrustResult<ValueRef> {
        match name {
            "println" => {
                let output = self.format_args(args, span)?;
                println!("{}", output);
                Ok(wrap(Value::Unit))
            }
            "print" => {
                let output = self.format_args(args, span)?;
                print!("{}", output);
                Ok(wrap(Value::Unit))
            }
            "eprintln" => {
                let output = self.format_args(args, span)?;
                eprintln!("{}", output);
                Ok(wrap(Value::Unit))
            }
            "eprint" => {
                let output = self.format_args(args, span)?;
                eprint!("{}", output);
                Ok(wrap(Value::Unit))
            }
            "format" => {
                let output = self.format_args(args, span)?;
                Ok(wrap(Value::Str(output)))
            }
            "vec" => {
                let mut items = Vec::new();
                for arg in args {
                    items.push(self.eval_expr(arg)?);
                }
                Ok(wrap(Value::Vec(items)))
            }
            "assert" => {
                if args.is_empty() {
                    return Err(runtime_err("assert! requires at least 1 argument", Some(span.clone())));
                }
                let cond = self.eval_expr(&args[0])?;
                if !self.is_truthy(&cond.borrow()) {
                    let msg = if args.len() > 1 {
                        self.format_args(&args[1..], span)?
                    } else {
                        "assertion failed".to_string()
                    };
                    return Err(runtime_err(msg, Some(span.clone())));
                }
                Ok(wrap(Value::Unit))
            }
            "assert_eq" => {
                if args.len() < 2 {
                    return Err(runtime_err("assert_eq! requires 2 arguments", Some(span.clone())));
                }
                let a = self.eval_expr(&args[0])?;
                let b = self.eval_expr(&args[1])?;
                let av = format!("{}", a.borrow());
                let bv = format!("{}", b.borrow());
                if av != bv {
                    return Err(runtime_err(
                        format!("assertion failed: {} != {}", av, bv),
                        Some(span.clone()),
                    ));
                }
                Ok(wrap(Value::Unit))
            }
            "panic" => {
                let msg = if args.is_empty() {
                    "explicit panic".to_string()
                } else {
                    self.format_args(args, span)?
                };
                Err(runtime_err(format!("panic: {}", msg), Some(span.clone())))
            }
            "todo" => {
                let msg = if args.is_empty() {
                    "not yet implemented".to_string()
                } else {
                    self.format_args(args, span)?
                };
                Err(runtime_err(format!("todo: {}", msg), Some(span.clone())))
            }
            _ => Err(runtime_err(format!("unknown macro '{}'", name), Some(span.clone()))),
        }
    }

    fn format_args(&mut self, args: &[Expr], span: &Span) -> CrustResult<String> {
        if args.is_empty() {
            return Ok(String::new());
        }
        let fmt_val = self.eval_expr(&args[0])?;
        let fmt_str = match fmt_val.borrow().clone() {
            Value::Str(s) => s,
            other => return Ok(format!("{}", other)),
        };

        let mut values = Vec::new();
        for arg in &args[1..] {
            values.push(self.eval_expr(arg)?);
        }

        // Apply format string substitution
        let mut result = String::new();
        let chars: Vec<char> = fmt_str.chars().collect();
        let mut i = 0;
        let mut val_idx = 0;

        while i < chars.len() {
            if chars[i] == '{' {
                if i + 1 < chars.len() && chars[i + 1] == '{' {
                    result.push('{');
                    i += 2;
                    continue;
                }
                // Find closing }
                let start = i;
                i += 1;
                let mut spec = String::new();
                while i < chars.len() && chars[i] != '}' {
                    spec.push(chars[i]);
                    i += 1;
                }
                if i < chars.len() { i += 1; } // consume }

                let val = values.get(val_idx).map(|v| v.borrow().clone()).unwrap_or(Value::Unit);
                val_idx += 1;

                // Handle format specifiers
                if spec.is_empty() || spec == "" {
                    result.push_str(&format!("{}", val));
                } else if spec.starts_with(':') {
                    let spec = &spec[1..];
                    if spec == "?" {
                        result.push_str(&format!("{:?}", format!("{}", val)));
                    } else if spec.ends_with('e') {
                        if let Value::Float(f) = val { result.push_str(&format!("{:e}", f)); }
                        else { result.push_str(&format!("{}", val)); }
                    } else if spec.ends_with('b') {
                        if let Value::Int(n) = val { result.push_str(&format!("{:b}", n)); }
                        else { result.push_str(&format!("{}", val)); }
                    } else if spec.ends_with('x') {
                        if let Value::Int(n) = val { result.push_str(&format!("{:x}", n)); }
                        else { result.push_str(&format!("{}", val)); }
                    } else if spec.ends_with('X') {
                        if let Value::Int(n) = val { result.push_str(&format!("{:X}", n)); }
                        else { result.push_str(&format!("{}", val)); }
                    } else if spec.ends_with('o') {
                        if let Value::Int(n) = val { result.push_str(&format!("{:o}", n)); }
                        else { result.push_str(&format!("{}", val)); }
                    } else {
                        result.push_str(&format!("{}", val));
                    }
                } else {
                    // Named arg or positional — just use next value
                    result.push_str(&format!("{}", val));
                }
            } else if chars[i] == '}' && i + 1 < chars.len() && chars[i + 1] == '}' {
                result.push('}');
                i += 2;
            } else {
                result.push(chars[i]);
                i += 1;
            }
        }
        Ok(result)
    }
}
