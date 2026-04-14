use crate::ast::*;
use crate::error::{CrustError, CrustResult, Span};
use crate::lexer::{Token, TokenKind};

pub struct Parser {
    tokens: Vec<Token>,
    pos: usize,
}

impl Parser {
    pub fn new(tokens: Vec<Token>) -> Self {
        Parser { tokens, pos: 0 }
    }

    fn peek(&self) -> &TokenKind {
        &self.tokens[self.pos].kind
    }

    fn peek_tok(&self) -> &Token {
        &self.tokens[self.pos]
    }

    fn span(&self) -> Span {
        self.tokens[self.pos].span.clone()
    }

    fn advance(&mut self) -> &Token {
        let tok = &self.tokens[self.pos];
        if self.pos + 1 < self.tokens.len() {
            self.pos += 1;
        }
        tok
    }

    fn expect(&mut self, kind: &TokenKind) -> CrustResult<Span> {
        if self.peek() == kind {
            let s = self.span();
            self.advance();
            Ok(s)
        } else {
            Err(CrustError::ParseError {
                msg: format!("expected {:?}, got {:?}", kind, self.peek()),
                span: self.span(),
            })
        }
    }

    fn check(&self, kind: &TokenKind) -> bool {
        self.peek() == kind
    }

    fn eat(&mut self, kind: &TokenKind) -> bool {
        if self.peek() == kind {
            self.advance();
            true
        } else {
            false
        }
    }

    pub fn parse_program(&mut self) -> CrustResult<Program> {
        let mut items = Vec::new();
        while !self.check(&TokenKind::Eof) {
            // Skip attribute lines like #[...]
            if self.check(&TokenKind::Hash) {
                self.advance();
                if self.eat(&TokenKind::LBracket) {
                    let mut depth = 1;
                    while depth > 0 && !self.check(&TokenKind::Eof) {
                        if self.eat(&TokenKind::LBracket) { depth += 1; }
                        else if self.eat(&TokenKind::RBracket) { depth -= 1; }
                        else { self.advance(); }
                    }
                }
                continue;
            }
            // pub modifier
            if self.check(&TokenKind::Pub) {
                self.advance();
            }
            match self.peek().clone() {
                TokenKind::Fn => items.push(Item::FnDecl(self.parse_fn()?)),
                TokenKind::Use => {
                    self.advance();
                    let span = self.span();
                    let mut path = Vec::new();
                    loop {
                        match self.peek().clone() {
                            TokenKind::Ident(name) => { self.advance(); path.push(name); }
                            TokenKind::ColonColon => { self.advance(); }
                            _ => break,
                        }
                    }
                    self.eat(&TokenKind::Semi);
                    items.push(Item::Use(UseDecl { path, span }));
                }
                TokenKind::Eof => break,
                _ => {
                    // skip unknown top-level tokens
                    self.advance();
                }
            }
        }
        Ok(Program { items })
    }

    fn parse_fn(&mut self) -> CrustResult<FnDecl> {
        let span = self.span();
        self.expect(&TokenKind::Fn)?;
        let name = match self.peek().clone() {
            TokenKind::Ident(n) => { self.advance(); n }
            _ => return Err(CrustError::ParseError {
                msg: format!("expected function name, got {:?}", self.peek()),
                span: self.span(),
            }),
        };
        // generic params (skip for now)
        if self.check(&TokenKind::Lt) {
            let mut depth = 1;
            self.advance();
            while depth > 0 && !self.check(&TokenKind::Eof) {
                if self.eat(&TokenKind::Lt) { depth += 1; }
                else if self.eat(&TokenKind::Gt) { depth -= 1; }
                else { self.advance(); }
            }
        }
        self.expect(&TokenKind::LParen)?;
        let params = self.parse_params()?;
        self.expect(&TokenKind::RParen)?;
        let ret_type = if self.eat(&TokenKind::Arrow) {
            Some(self.parse_type()?)
        } else {
            None
        };
        // where clause (skip)
        if self.check(&TokenKind::Where) {
            self.advance();
            while !self.check(&TokenKind::LBrace) && !self.check(&TokenKind::Eof) {
                self.advance();
            }
        }
        let body = self.parse_block()?;
        Ok(FnDecl { name, params, ret_type, body, span })
    }

    fn parse_params(&mut self) -> CrustResult<Vec<Param>> {
        let mut params = Vec::new();
        while !self.check(&TokenKind::RParen) && !self.check(&TokenKind::Eof) {
            // skip self
            if self.check(&TokenKind::Ampersand) {
                self.advance();
                if self.check(&TokenKind::Mut) { self.advance(); }
                if self.check(&TokenKind::Self_) { self.advance(); self.eat(&TokenKind::Comma); continue; }
            }
            if self.check(&TokenKind::Self_) { self.advance(); self.eat(&TokenKind::Comma); continue; }
            let span = self.span();
            let mutable = self.eat(&TokenKind::Mut);
            let name = match self.peek().clone() {
                TokenKind::Ident(n) => { self.advance(); n }
                TokenKind::Underscore => { self.advance(); "_".to_string() }
                _ => break,
            };
            self.expect(&TokenKind::Colon)?;
            let ty = self.parse_type()?;
            params.push(Param { name, ty, mutable, span });
            self.eat(&TokenKind::Comma);
        }
        Ok(params)
    }

    fn parse_type(&mut self) -> CrustResult<TypeExpr> {
        // &T or &mut T
        if self.eat(&TokenKind::Ampersand) {
            let mutable = self.eat(&TokenKind::Mut);
            // skip lifetime 'a
            let inner = self.parse_type()?;
            return Ok(TypeExpr::Ref(Box::new(inner), mutable));
        }
        // ()
        if self.check(&TokenKind::LParen) {
            self.advance();
            if self.eat(&TokenKind::RParen) {
                return Ok(TypeExpr::Unit);
            }
            // tuple type (skip for simplicity)
            let t = self.parse_type()?;
            self.eat(&TokenKind::Comma);
            self.expect(&TokenKind::RParen)?;
            return Ok(t);
        }
        let name = match self.peek().clone() {
            TokenKind::Ident(n) => { self.advance(); n }
            TokenKind::Self_ => { self.advance(); "Self".to_string() }
            _ => return Err(CrustError::ParseError {
                msg: format!("expected type, got {:?}", self.peek()),
                span: self.span(),
            }),
        };
        // handle path like std::string::String
        let mut full = name;
        while self.check(&TokenKind::ColonColon) {
            self.advance();
            match self.peek().clone() {
                TokenKind::Ident(n) => { self.advance(); full = n; }
                _ => break,
            }
        }
        // generic args
        let mut args = Vec::new();
        if self.check(&TokenKind::Lt) {
            self.advance();
            while !self.check(&TokenKind::Gt) && !self.check(&TokenKind::Eof) {
                args.push(self.parse_type()?);
                self.eat(&TokenKind::Comma);
            }
            self.eat(&TokenKind::Gt);
        }
        Ok(TypeExpr::Named(full, args))
    }

    fn parse_block(&mut self) -> CrustResult<Block> {
        let span = self.span();
        self.expect(&TokenKind::LBrace)?;
        let mut stmts = Vec::new();
        while !self.check(&TokenKind::RBrace) && !self.check(&TokenKind::Eof) {
            stmts.push(self.parse_stmt()?);
        }
        self.expect(&TokenKind::RBrace)?;
        Ok(Block { stmts, span })
    }

    fn parse_stmt(&mut self) -> CrustResult<Stmt> {
        match self.peek().clone() {
            TokenKind::Let => self.parse_let(),
            TokenKind::Return => {
                let span = self.span();
                self.advance();
                if self.check(&TokenKind::Semi) {
                    self.advance();
                    Ok(Stmt::Return(None, span))
                } else {
                    let expr = self.parse_expr()?;
                    self.eat(&TokenKind::Semi);
                    Ok(Stmt::Return(Some(expr), span))
                }
            }
            TokenKind::Break => {
                let span = self.span();
                self.advance();
                self.eat(&TokenKind::Semi);
                Ok(Stmt::Break(span))
            }
            TokenKind::Continue => {
                let span = self.span();
                self.advance();
                self.eat(&TokenKind::Semi);
                Ok(Stmt::Continue(span))
            }
            // Skip attribute macros #[...]
            TokenKind::Hash => {
                self.advance();
                if self.eat(&TokenKind::LBracket) {
                    let mut depth = 1;
                    while depth > 0 && !self.check(&TokenKind::Eof) {
                        if self.eat(&TokenKind::LBracket) { depth += 1; }
                        else if self.eat(&TokenKind::RBracket) { depth -= 1; }
                        else { self.advance(); }
                    }
                }
                self.parse_stmt()
            }
            _ => {
                let expr = self.parse_expr()?;
                // If a block expression (if/while/for/loop/bare block) — no semicolon needed
                let is_block = matches!(expr,
                    Expr::If(..) | Expr::While(..) | Expr::Loop(..) |
                    Expr::For(..) | Expr::Block(..)
                );
                if self.eat(&TokenKind::Semi) {
                    Ok(Stmt::Semi(expr))
                } else if is_block {
                    Ok(Stmt::Expr(expr))
                } else {
                    // expression statement without semicolon = last value
                    Ok(Stmt::Expr(expr))
                }
            }
        }
    }

    fn parse_let(&mut self) -> CrustResult<Stmt> {
        let span = self.span();
        self.expect(&TokenKind::Let)?;
        let mutable = self.eat(&TokenKind::Mut);
        let name = match self.peek().clone() {
            TokenKind::Ident(n) => { self.advance(); n }
            TokenKind::Underscore => { self.advance(); "_".to_string() }
            _ => return Err(CrustError::ParseError {
                msg: format!("expected identifier in let, got {:?}", self.peek()),
                span: self.span(),
            }),
        };
        let ty = if self.eat(&TokenKind::Colon) {
            Some(self.parse_type()?)
        } else {
            None
        };
        let init = if self.eat(&TokenKind::Eq) {
            Some(self.parse_expr()?)
        } else {
            None
        };
        self.eat(&TokenKind::Semi);
        Ok(Stmt::Let(LetStmt { name, mutable, ty, init, span }))
    }

    // Expression parsing with precedence climbing
    fn parse_expr(&mut self) -> CrustResult<Expr> {
        self.parse_assign()
    }

    fn parse_assign(&mut self) -> CrustResult<Expr> {
        let lhs = self.parse_range()?;
        let span = self.span();
        if self.eat(&TokenKind::Eq) {
            let rhs = self.parse_assign()?;
            return Ok(Expr::Assign(Box::new(lhs), Box::new(rhs), span));
        }
        // compound assignment
        let op = match self.peek() {
            TokenKind::PlusEq  => Some(BinOp::Add),
            TokenKind::MinusEq => Some(BinOp::Sub),
            TokenKind::StarEq  => Some(BinOp::Mul),
            TokenKind::SlashEq => Some(BinOp::Div),
            TokenKind::PercentEq => Some(BinOp::Rem),
            _ => None,
        };
        if let Some(op) = op {
            self.advance();
            let rhs = self.parse_assign()?;
            return Ok(Expr::CompoundAssign(op, Box::new(lhs), Box::new(rhs), span));
        }
        Ok(lhs)
    }

    fn parse_range(&mut self) -> CrustResult<Expr> {
        let start = self.parse_or()?;
        let span = self.span();
        if self.check(&TokenKind::DotDot) || self.check(&TokenKind::DotDotEq) {
            let inclusive = self.peek() == &TokenKind::DotDotEq;
            self.advance();
            // Check if there's an end expression
            if !self.check(&TokenKind::Semi) && !self.check(&TokenKind::RBracket) && !self.check(&TokenKind::RBrace) {
                let end = self.parse_or()?;
                return Ok(Expr::Range(Some(Box::new(start)), Some(Box::new(end)), inclusive, span));
            }
            return Ok(Expr::Range(Some(Box::new(start)), None, inclusive, span));
        }
        Ok(start)
    }

    fn parse_or(&mut self) -> CrustResult<Expr> {
        let mut lhs = self.parse_and()?;
        while self.check(&TokenKind::OrOr) {
            let span = self.span();
            self.advance();
            let rhs = self.parse_and()?;
            lhs = Expr::BinOp(BinOp::Or, Box::new(lhs), Box::new(rhs), span);
        }
        Ok(lhs)
    }

    fn parse_and(&mut self) -> CrustResult<Expr> {
        let mut lhs = self.parse_cmp()?;
        while self.check(&TokenKind::AndAnd) {
            let span = self.span();
            self.advance();
            let rhs = self.parse_cmp()?;
            lhs = Expr::BinOp(BinOp::And, Box::new(lhs), Box::new(rhs), span);
        }
        Ok(lhs)
    }

    fn parse_cmp(&mut self) -> CrustResult<Expr> {
        let mut lhs = self.parse_bitor()?;
        loop {
            let span = self.span();
            let op = match self.peek() {
                TokenKind::EqEq  => BinOp::Eq,
                TokenKind::BangEq => BinOp::Ne,
                TokenKind::Lt    => BinOp::Lt,
                TokenKind::LtEq  => BinOp::Le,
                TokenKind::Gt    => BinOp::Gt,
                TokenKind::GtEq  => BinOp::Ge,
                _ => break,
            };
            self.advance();
            let rhs = self.parse_bitor()?;
            lhs = Expr::BinOp(op, Box::new(lhs), Box::new(rhs), span);
        }
        Ok(lhs)
    }

    fn parse_bitor(&mut self) -> CrustResult<Expr> {
        let mut lhs = self.parse_bitxor()?;
        while self.check(&TokenKind::Pipe) {
            let span = self.span();
            self.advance();
            let rhs = self.parse_bitxor()?;
            lhs = Expr::BinOp(BinOp::BitOr, Box::new(lhs), Box::new(rhs), span);
        }
        Ok(lhs)
    }

    fn parse_bitxor(&mut self) -> CrustResult<Expr> {
        let mut lhs = self.parse_bitand()?;
        while self.check(&TokenKind::Caret) {
            let span = self.span();
            self.advance();
            let rhs = self.parse_bitand()?;
            lhs = Expr::BinOp(BinOp::BitXor, Box::new(lhs), Box::new(rhs), span);
        }
        Ok(lhs)
    }

    fn parse_bitand(&mut self) -> CrustResult<Expr> {
        let mut lhs = self.parse_shift()?;
        while self.check(&TokenKind::Ampersand) {
            let span = self.span();
            self.advance();
            let rhs = self.parse_shift()?;
            lhs = Expr::BinOp(BinOp::BitAnd, Box::new(lhs), Box::new(rhs), span);
        }
        Ok(lhs)
    }

    fn parse_shift(&mut self) -> CrustResult<Expr> {
        let mut lhs = self.parse_add()?;
        loop {
            let span = self.span();
            let op = match self.peek() {
                TokenKind::Shl => BinOp::Shl,
                TokenKind::Shr => BinOp::Shr,
                _ => break,
            };
            self.advance();
            let rhs = self.parse_add()?;
            lhs = Expr::BinOp(op, Box::new(lhs), Box::new(rhs), span);
        }
        Ok(lhs)
    }

    fn parse_add(&mut self) -> CrustResult<Expr> {
        let mut lhs = self.parse_mul()?;
        loop {
            let span = self.span();
            let op = match self.peek() {
                TokenKind::Plus  => BinOp::Add,
                TokenKind::Minus => BinOp::Sub,
                _ => break,
            };
            self.advance();
            let rhs = self.parse_mul()?;
            lhs = Expr::BinOp(op, Box::new(lhs), Box::new(rhs), span);
        }
        Ok(lhs)
    }

    fn parse_mul(&mut self) -> CrustResult<Expr> {
        let mut lhs = self.parse_unary()?;
        loop {
            let span = self.span();
            let op = match self.peek() {
                TokenKind::Star    => BinOp::Mul,
                TokenKind::Slash   => BinOp::Div,
                TokenKind::Percent => BinOp::Rem,
                _ => break,
            };
            self.advance();
            let rhs = self.parse_unary()?;
            lhs = Expr::BinOp(op, Box::new(lhs), Box::new(rhs), span);
        }
        Ok(lhs)
    }

    fn parse_unary(&mut self) -> CrustResult<Expr> {
        let span = self.span();
        if self.eat(&TokenKind::Minus) {
            let e = self.parse_unary()?;
            return Ok(Expr::UnOp(UnOp::Neg, Box::new(e), span));
        }
        if self.eat(&TokenKind::Bang) {
            let e = self.parse_unary()?;
            return Ok(Expr::UnOp(UnOp::Not, Box::new(e), span));
        }
        if self.eat(&TokenKind::Star) {
            let e = self.parse_unary()?;
            return Ok(Expr::UnOp(UnOp::Deref, Box::new(e), span));
        }
        if self.eat(&TokenKind::Ampersand) {
            let mutable = self.eat(&TokenKind::Mut);
            let e = self.parse_unary()?;
            return Ok(Expr::UnOp(if mutable { UnOp::RefMut } else { UnOp::Ref }, Box::new(e), span));
        }
        self.parse_postfix()
    }

    fn parse_postfix(&mut self) -> CrustResult<Expr> {
        let mut expr = self.parse_primary()?;
        loop {
            let span = self.span();
            if self.eat(&TokenKind::Dot) {
                match self.peek().clone() {
                    TokenKind::Ident(field) => {
                        self.advance();
                        if self.eat(&TokenKind::LParen) {
                            let args = self.parse_args()?;
                            self.expect(&TokenKind::RParen)?;
                            expr = Expr::MethodCall(Box::new(expr), field, args, span);
                        } else {
                            expr = Expr::FieldAccess(Box::new(expr), field, span);
                        }
                    }
                    TokenKind::IntLit(n) => {
                        let n = n;
                        self.advance();
                        expr = Expr::FieldAccess(Box::new(expr), n.to_string(), span);
                    }
                    _ => break,
                }
            } else if self.eat(&TokenKind::LBracket) {
                let idx = self.parse_expr()?;
                self.expect(&TokenKind::RBracket)?;
                expr = Expr::Index(Box::new(expr), Box::new(idx), span);
            } else if self.check(&TokenKind::LParen) {
                self.advance();
                let args = self.parse_args()?;
                self.expect(&TokenKind::RParen)?;
                expr = Expr::Call(Box::new(expr), args, span);
            } else {
                break;
            }
        }
        Ok(expr)
    }

    fn parse_primary(&mut self) -> CrustResult<Expr> {
        let span = self.span();
        match self.peek().clone() {
            TokenKind::IntLit(n) => { self.advance(); Ok(Expr::Lit(Lit::Int(n), span)) }
            TokenKind::FloatLit(f) => { self.advance(); Ok(Expr::Lit(Lit::Float(f), span)) }
            TokenKind::StringLit(s) => { self.advance(); Ok(Expr::Lit(Lit::Str(s), span)) }
            TokenKind::True => { self.advance(); Ok(Expr::Lit(Lit::Bool(true), span)) }
            TokenKind::False => { self.advance(); Ok(Expr::Lit(Lit::Bool(false), span)) }
            TokenKind::CharLit(c) => { self.advance(); Ok(Expr::Lit(Lit::Char(c), span)) }

            TokenKind::Macro(name) => {
                let name = name.clone();
                self.advance();
                // Macros can use () or [] or {} as delimiters
                let close = if self.eat(&TokenKind::LParen) {
                    TokenKind::RParen
                } else if self.eat(&TokenKind::LBracket) {
                    TokenKind::RBracket
                } else if self.eat(&TokenKind::LBrace) {
                    TokenKind::RBrace
                } else {
                    return Err(CrustError::ParseError {
                        msg: format!("expected '(', '[', or '{{' after macro '{}'", name),
                        span: self.span(),
                    });
                };
                let args = self.parse_macro_args_until(&close)?;
                self.expect(&close)?;
                Ok(Expr::MacroCall(name, args, span))
            }

            TokenKind::Ident(name) => {
                let name = name.clone();
                self.advance();
                // path like Vec::new() or HashMap::new()
                if self.check(&TokenKind::ColonColon) {
                    self.advance();
                    match self.peek().clone() {
                        TokenKind::Ident(method) => {
                            let method = method.clone();
                            self.advance();
                            if self.eat(&TokenKind::LParen) {
                                let args = self.parse_args()?;
                                self.expect(&TokenKind::RParen)?;
                                // Represent as a call on a synthetic path expr
                                let path_expr = Expr::FieldAccess(
                                    Box::new(Expr::Ident(name, span.clone())),
                                    method,
                                    span.clone(),
                                );
                                return Ok(Expr::Call(Box::new(path_expr), args, span));
                            }
                            return Ok(Expr::FieldAccess(
                                Box::new(Expr::Ident(name, span.clone())),
                                method,
                                span,
                            ));
                        }
                        _ => {}
                    }
                }
                Ok(Expr::Ident(name, span))
            }

            TokenKind::LParen => {
                self.advance();
                if self.eat(&TokenKind::RParen) {
                    return Ok(Expr::Lit(Lit::Unit, span));
                }
                let e = self.parse_expr()?;
                if self.eat(&TokenKind::Comma) {
                    // tuple
                    let mut elems = vec![e];
                    while !self.check(&TokenKind::RParen) && !self.check(&TokenKind::Eof) {
                        elems.push(self.parse_expr()?);
                        self.eat(&TokenKind::Comma);
                    }
                    self.expect(&TokenKind::RParen)?;
                    return Ok(Expr::Tuple(elems, span));
                }
                self.expect(&TokenKind::RParen)?;
                Ok(e)
            }

            TokenKind::LBracket => {
                self.advance();
                let mut elems = Vec::new();
                while !self.check(&TokenKind::RBracket) && !self.check(&TokenKind::Eof) {
                    elems.push(self.parse_expr()?);
                    self.eat(&TokenKind::Comma);
                }
                self.expect(&TokenKind::RBracket)?;
                Ok(Expr::Array(elems, span))
            }

            TokenKind::LBrace => {
                let block = self.parse_block()?;
                Ok(Expr::Block(block))
            }

            TokenKind::If => self.parse_if(),
            TokenKind::While => self.parse_while(),
            TokenKind::Loop => self.parse_loop_expr(),
            TokenKind::For => self.parse_for(),
            TokenKind::Return => {
                self.advance();
                if self.check(&TokenKind::Semi) || self.check(&TokenKind::RBrace) {
                    Ok(Expr::Return(Box::new(Expr::Lit(Lit::Unit, span.clone())), span))
                } else {
                    let e = self.parse_expr()?;
                    Ok(Expr::Return(Box::new(e), span))
                }
            }
            TokenKind::Break => {
                self.advance();
                Ok(Expr::Break(span))
            }
            TokenKind::Continue => {
                self.advance();
                Ok(Expr::Continue(span))
            }

            _ => Err(CrustError::ParseError {
                msg: format!("unexpected token {:?}", self.peek()),
                span,
            }),
        }
    }

    fn parse_if(&mut self) -> CrustResult<Expr> {
        let span = self.span();
        self.expect(&TokenKind::If)?;
        let cond = self.parse_expr()?;
        let then_block = self.parse_block()?;
        let else_branch = if self.eat(&TokenKind::Else) {
            if self.check(&TokenKind::If) {
                let elif = self.parse_if()?;
                Some(Box::new(IfElse::ElseIf(elif)))
            } else {
                let else_block = self.parse_block()?;
                Some(Box::new(IfElse::Else(else_block)))
            }
        } else {
            None
        };
        Ok(Expr::If(Box::new(cond), then_block, else_branch, span))
    }

    fn parse_while(&mut self) -> CrustResult<Expr> {
        let span = self.span();
        self.expect(&TokenKind::While)?;
        let cond = self.parse_expr()?;
        let body = self.parse_block()?;
        Ok(Expr::While(Box::new(cond), body, span))
    }

    fn parse_loop_expr(&mut self) -> CrustResult<Expr> {
        let span = self.span();
        self.expect(&TokenKind::Loop)?;
        let body = self.parse_block()?;
        Ok(Expr::Loop(body, span))
    }

    fn parse_for(&mut self) -> CrustResult<Expr> {
        let span = self.span();
        self.expect(&TokenKind::For)?;
        let var = match self.peek().clone() {
            TokenKind::Ident(n) => { self.advance(); n }
            TokenKind::Underscore => { self.advance(); "_".to_string() }
            _ => return Err(CrustError::ParseError {
                msg: "expected identifier in for".into(),
                span: self.span(),
            }),
        };
        self.expect(&TokenKind::In)?;
        let iter = self.parse_expr()?;
        let body = self.parse_block()?;
        Ok(Expr::For(var, Box::new(iter), body, span))
    }

    fn parse_args(&mut self) -> CrustResult<Vec<Expr>> {
        let mut args = Vec::new();
        while !self.check(&TokenKind::RParen) && !self.check(&TokenKind::Eof) {
            args.push(self.parse_expr()?);
            if !self.eat(&TokenKind::Comma) { break; }
        }
        Ok(args)
    }

    // Macro args: first arg may be a format string, rest are expressions
    fn parse_macro_args(&mut self) -> CrustResult<Vec<Expr>> {
        self.parse_args()
    }

    fn parse_macro_args_until(&mut self, close: &TokenKind) -> CrustResult<Vec<Expr>> {
        let mut args = Vec::new();
        while self.peek() != close && !self.check(&TokenKind::Eof) {
            args.push(self.parse_expr()?);
            if !self.eat(&TokenKind::Comma) { break; }
        }
        Ok(args)
    }
}

// Need to handle `main` as a valid function name identifier
impl TokenKind {
    #[allow(dead_code)]
    pub fn is_ident_like(&self) -> bool {
        matches!(self, TokenKind::Ident(_) | TokenKind::Self_ | TokenKind::Super)
    }
}

// Note: "main" stays as Ident("main") since keyword_or_ident doesn't intercept it.
