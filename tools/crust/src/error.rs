use std::fmt;

#[derive(Debug, Clone)]
pub struct Span {
    pub line: usize,
    pub col: usize,
}

impl Span {
    pub fn new(line: usize, col: usize) -> Self {
        Span { line, col }
    }
}

impl fmt::Display for Span {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}", self.line, self.col)
    }
}

#[derive(Debug, Clone)]
pub enum CrustError {
    LexError { msg: String, span: Span },
    ParseError { msg: String, span: Span },
    RuntimeError { msg: String, span: Option<Span> },
    TypeError { msg: String, span: Option<Span> },
    Return(crate::interpreter::Value),
    Break,
    Continue,
}

impl fmt::Display for CrustError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            CrustError::LexError { msg, span } => write!(f, "lex error at {}: {}", span, msg),
            CrustError::ParseError { msg, span } => write!(f, "parse error at {}: {}", span, msg),
            CrustError::RuntimeError { msg, span } => match span {
                Some(s) => write!(f, "runtime error at {}: {}", s, msg),
                None => write!(f, "runtime error: {}", msg),
            },
            CrustError::TypeError { msg, span } => match span {
                Some(s) => write!(f, "type error at {}: {}", s, msg),
                None => write!(f, "type error: {}", msg),
            },
            CrustError::Return(_) => write!(f, "<return>"),
            CrustError::Break => write!(f, "<break>"),
            CrustError::Continue => write!(f, "<continue>"),
        }
    }
}

pub type CrustResult<T> = Result<T, CrustError>;
