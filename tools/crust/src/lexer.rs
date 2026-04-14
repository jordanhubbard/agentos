use crate::error::{CrustError, CrustResult, Span};

#[derive(Debug, Clone, PartialEq)]
pub enum TokenKind {
    // Literals
    IntLit(i64),
    FloatLit(f64),
    StringLit(String),
    BoolLit(bool),
    CharLit(char),

    // Identifiers
    Ident(String),

    // Keywords
    Fn, Let, Mut, If, Else, While, Loop, For, In, Return,
    Struct, Enum, Impl, Pub, Use, Mod, Match, Where,
    Break, Continue, As, Const, Static, Type, Trait,
    Self_, Super, Crate, Ref, Box_, Move,
    True, False,

    // Operators
    Plus, Minus, Star, Slash, Percent,
    Eq, EqEq, BangEq, Lt, LtEq, Gt, GtEq,
    And, AndAnd, Or, OrOr, Bang,
    Ampersand, Pipe, Caret, Shl, Shr,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AndEq, OrEq, CaretEq, ShlEq, ShrEq,
    Arrow, FatArrow, DotDot, DotDotEq,
    Question, Colon, ColonColon,

    // Delimiters
    LParen, RParen,
    LBrace, RBrace,
    LBracket, RBracket,
    Comma, Dot, Semi,
    Hash, At, Underscore,

    // Macros (identifiers followed by !)
    Macro(String),

    // Special
    Eof,
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokenKind,
    pub span: Span,
}

impl Token {
    fn new(kind: TokenKind, line: usize, col: usize) -> Self {
        Token { kind, span: Span::new(line, col) }
    }
}

pub struct Lexer {
    input: Vec<char>,
    pos: usize,
    line: usize,
    col: usize,
}

impl Lexer {
    pub fn new(source: &str) -> Self {
        Lexer {
            input: source.chars().collect(),
            pos: 0,
            line: 1,
            col: 1,
        }
    }

    fn peek(&self) -> Option<char> {
        self.input.get(self.pos).copied()
    }

    fn peek2(&self) -> Option<char> {
        self.input.get(self.pos + 1).copied()
    }

    fn advance(&mut self) -> Option<char> {
        let c = self.input.get(self.pos).copied();
        if let Some(ch) = c {
            self.pos += 1;
            if ch == '\n' {
                self.line += 1;
                self.col = 1;
            } else {
                self.col += 1;
            }
        }
        c
    }

    fn skip_whitespace_and_comments(&mut self) {
        loop {
            // Skip whitespace
            while matches!(self.peek(), Some(' ') | Some('\t') | Some('\r') | Some('\n')) {
                self.advance();
            }
            // Skip line comments
            if self.peek() == Some('/') && self.peek2() == Some('/') {
                while self.peek().is_some() && self.peek() != Some('\n') {
                    self.advance();
                }
                continue;
            }
            // Skip block comments
            if self.peek() == Some('/') && self.peek2() == Some('*') {
                self.advance(); self.advance();
                loop {
                    if self.peek() == Some('*') && self.peek2() == Some('/') {
                        self.advance(); self.advance();
                        break;
                    }
                    if self.advance().is_none() { break; }
                }
                continue;
            }
            break;
        }
    }

    fn lex_string(&mut self, start_line: usize, start_col: usize) -> CrustResult<Token> {
        // consume opening "
        self.advance();
        let mut s = String::new();
        loop {
            match self.peek() {
                None => return Err(CrustError::LexError {
                    msg: "unterminated string literal".into(),
                    span: Span::new(start_line, start_col),
                }),
                Some('"') => { self.advance(); break; }
                Some('\\') => {
                    self.advance();
                    match self.advance() {
                        Some('n') => s.push('\n'),
                        Some('t') => s.push('\t'),
                        Some('r') => s.push('\r'),
                        Some('\\') => s.push('\\'),
                        Some('"') => s.push('"'),
                        Some('0') => s.push('\0'),
                        Some(c) => s.push(c),
                        None => return Err(CrustError::LexError {
                            msg: "unexpected end of escape".into(),
                            span: Span::new(self.line, self.col),
                        }),
                    }
                }
                Some(c) => { s.push(c); self.advance(); }
            }
        }
        Ok(Token::new(TokenKind::StringLit(s), start_line, start_col))
    }

    fn lex_char(&mut self, start_line: usize, start_col: usize) -> CrustResult<Token> {
        self.advance(); // consume '
        let c = match self.advance() {
            None => return Err(CrustError::LexError {
                msg: "unterminated char literal".into(),
                span: Span::new(start_line, start_col),
            }),
            Some('\\') => match self.advance() {
                Some('n') => '\n',
                Some('t') => '\t',
                Some('r') => '\r',
                Some('\\') => '\\',
                Some('\'') => '\'',
                Some('0') => '\0',
                Some(c) => c,
                None => return Err(CrustError::LexError {
                    msg: "bad escape in char".into(),
                    span: Span::new(self.line, self.col),
                }),
            },
            Some(c) => c,
        };
        if self.peek() != Some('\'') {
            return Err(CrustError::LexError {
                msg: "expected closing ' for char literal".into(),
                span: Span::new(self.line, self.col),
            });
        }
        self.advance();
        Ok(Token::new(TokenKind::CharLit(c), start_line, start_col))
    }

    fn lex_number(&mut self, start_line: usize, start_col: usize) -> Token {
        let mut s = String::new();
        let mut is_float = false;
        while matches!(self.peek(), Some('0'..='9') | Some('_')) {
            let c = self.advance().unwrap();
            if c != '_' { s.push(c); }
        }
        if self.peek() == Some('.') && matches!(self.peek2(), Some('0'..='9')) {
            is_float = true;
            s.push('.');
            self.advance();
            while matches!(self.peek(), Some('0'..='9') | Some('_')) {
                let c = self.advance().unwrap();
                if c != '_' { s.push(c); }
            }
        }
        // optional exponent
        if matches!(self.peek(), Some('e') | Some('E')) {
            is_float = true;
            s.push('e');
            self.advance();
            if matches!(self.peek(), Some('+') | Some('-')) {
                s.push(self.advance().unwrap());
            }
            while matches!(self.peek(), Some('0'..='9')) {
                s.push(self.advance().unwrap());
            }
        }
        // consume type suffix like i64, f64, usize, etc.
        while matches!(self.peek(), Some('a'..='z') | Some('A'..='Z') | Some('0'..='9')) {
            self.advance();
        }
        if is_float {
            Token::new(TokenKind::FloatLit(s.parse().unwrap_or(0.0)), start_line, start_col)
        } else {
            Token::new(TokenKind::IntLit(s.parse().unwrap_or(0)), start_line, start_col)
        }
    }

    fn keyword_or_ident(s: String) -> TokenKind {
        match s.as_str() {
            "fn"       => TokenKind::Fn,
            "let"      => TokenKind::Let,
            "mut"      => TokenKind::Mut,
            "if"       => TokenKind::If,
            "else"     => TokenKind::Else,
            "while"    => TokenKind::While,
            "loop"     => TokenKind::Loop,
            "for"      => TokenKind::For,
            "in"       => TokenKind::In,
            "return"   => TokenKind::Return,
            "struct"   => TokenKind::Struct,
            "enum"     => TokenKind::Enum,
            "impl"     => TokenKind::Impl,
            "pub"      => TokenKind::Pub,
            "use"      => TokenKind::Use,
            "mod"      => TokenKind::Mod,
            "match"    => TokenKind::Match,
            "where"    => TokenKind::Where,
            "break"    => TokenKind::Break,
            "continue" => TokenKind::Continue,
            "as"       => TokenKind::As,
            "const"    => TokenKind::Const,
            "static"   => TokenKind::Static,
            "type"     => TokenKind::Type,
            "trait"    => TokenKind::Trait,
            "self"     => TokenKind::Self_,
            "super"    => TokenKind::Super,
            "crate"    => TokenKind::Crate,
            "ref"      => TokenKind::Ref,
            "box"      => TokenKind::Box_,
            "move"     => TokenKind::Move,
            "true"     => TokenKind::True,
            "false"    => TokenKind::False,
            "_"        => TokenKind::Underscore,
            _          => TokenKind::Ident(s),
        }
    }

    pub fn tokenize(&mut self) -> CrustResult<Vec<Token>> {
        let mut tokens = Vec::new();
        loop {
            self.skip_whitespace_and_comments();
            let line = self.line;
            let col = self.col;
            match self.peek() {
                None => { tokens.push(Token::new(TokenKind::Eof, line, col)); break; }
                Some('"') => tokens.push(self.lex_string(line, col)?),
                Some('\'') => {
                    // Could be lifetime 'a or char literal
                    // If followed by ident and then not closing ', treat as lifetime (skip for now)
                    tokens.push(self.lex_char(line, col)?);
                }
                Some('0'..='9') => tokens.push(self.lex_number(line, col)),
                Some('a'..='z') | Some('A'..='Z') | Some('_') => {
                    let mut s = String::new();
                    while matches!(self.peek(), Some('a'..='z') | Some('A'..='Z') | Some('0'..='9') | Some('_')) {
                        s.push(self.advance().unwrap());
                    }
                    // Check for macro invocation: ident!
                    if self.peek() == Some('!') {
                        self.advance();
                        tokens.push(Token::new(TokenKind::Macro(s), line, col));
                    } else {
                        tokens.push(Token::new(Self::keyword_or_ident(s), line, col));
                    }
                }
                Some(c) => {
                    self.advance();
                    let tok = match c {
                        '+' => if self.peek() == Some('=') { self.advance(); TokenKind::PlusEq } else { TokenKind::Plus },
                        '-' => if self.peek() == Some('>') { self.advance(); TokenKind::Arrow }
                               else if self.peek() == Some('=') { self.advance(); TokenKind::MinusEq }
                               else { TokenKind::Minus },
                        '*' => if self.peek() == Some('=') { self.advance(); TokenKind::StarEq } else { TokenKind::Star },
                        '/' => if self.peek() == Some('=') { self.advance(); TokenKind::SlashEq } else { TokenKind::Slash },
                        '%' => if self.peek() == Some('=') { self.advance(); TokenKind::PercentEq } else { TokenKind::Percent },
                        '=' => if self.peek() == Some('=') { self.advance(); TokenKind::EqEq }
                               else if self.peek() == Some('>') { self.advance(); TokenKind::FatArrow }
                               else { TokenKind::Eq },
                        '!' => if self.peek() == Some('=') { self.advance(); TokenKind::BangEq } else { TokenKind::Bang },
                        '<' => if self.peek() == Some('=') { self.advance(); TokenKind::LtEq }
                               else if self.peek() == Some('<') {
                                   self.advance();
                                   if self.peek() == Some('=') { self.advance(); TokenKind::ShlEq } else { TokenKind::Shl }
                               } else { TokenKind::Lt },
                        '>' => if self.peek() == Some('=') { self.advance(); TokenKind::GtEq }
                               else if self.peek() == Some('>') {
                                   self.advance();
                                   if self.peek() == Some('=') { self.advance(); TokenKind::ShrEq } else { TokenKind::Shr }
                               } else { TokenKind::Gt },
                        '&' => if self.peek() == Some('&') { self.advance(); TokenKind::AndAnd }
                               else if self.peek() == Some('=') { self.advance(); TokenKind::AndEq }
                               else { TokenKind::Ampersand },
                        '|' => if self.peek() == Some('|') { self.advance(); TokenKind::OrOr }
                               else if self.peek() == Some('=') { self.advance(); TokenKind::OrEq }
                               else { TokenKind::Pipe },
                        '^' => if self.peek() == Some('=') { self.advance(); TokenKind::CaretEq } else { TokenKind::Caret },
                        '.' => if self.peek() == Some('.') {
                                   self.advance();
                                   if self.peek() == Some('=') { self.advance(); TokenKind::DotDotEq } else { TokenKind::DotDot }
                               } else { TokenKind::Dot },
                        ':' => if self.peek() == Some(':') { self.advance(); TokenKind::ColonColon } else { TokenKind::Colon },
                        '?' => TokenKind::Question,
                        '(' => TokenKind::LParen,
                        ')' => TokenKind::RParen,
                        '{' => TokenKind::LBrace,
                        '}' => TokenKind::RBrace,
                        '[' => TokenKind::LBracket,
                        ']' => TokenKind::RBracket,
                        ',' => TokenKind::Comma,
                        ';' => TokenKind::Semi,
                        '#' => TokenKind::Hash,
                        '@' => TokenKind::At,
                        _ => return Err(CrustError::LexError {
                            msg: format!("unexpected character '{}'", c),
                            span: Span::new(line, col),
                        }),
                    };
                    tokens.push(Token::new(tok, line, col));
                }
            }
        }
        Ok(tokens)
    }
}
