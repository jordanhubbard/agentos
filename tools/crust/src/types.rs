/// Basic type inference — v0.1 hack mode only.
/// In hack mode, all values are dynamically typed via Value enum.
/// This module provides structural type annotations for future pedantic mode.

#[derive(Debug, Clone, PartialEq)]
pub enum Type {
    I64,
    F64,
    Bool,
    Char,
    Str,       // &str (hack mode: same as String)
    String,
    Unit,
    Vec(Box<Type>),
    Tuple(Vec<Type>),
    Unknown,
}

impl Type {
    /// Infer from a value (best-effort)
    pub fn from_value(v: &crate::interpreter::Value) -> Self {
        match v {
            crate::interpreter::Value::Int(_)   => Type::I64,
            crate::interpreter::Value::Float(_) => Type::F64,
            crate::interpreter::Value::Bool(_)  => Type::Bool,
            crate::interpreter::Value::Char(_)  => Type::Char,
            crate::interpreter::Value::Str(_)   => Type::String,
            crate::interpreter::Value::Unit     => Type::Unit,
            crate::interpreter::Value::Vec(items) => {
                let elem = items.first()
                    .map(|v| Type::from_value(&v.borrow()))
                    .unwrap_or(Type::Unknown);
                Type::Vec(Box::new(elem))
            }
            crate::interpreter::Value::Tuple(items) => {
                Type::Tuple(items.iter().map(|v| Type::from_value(&v.borrow())).collect())
            }
            crate::interpreter::Value::Function(_) => Type::Unknown,
            crate::interpreter::Value::Builtin(_) => Type::Unknown,
        }
    }
}

impl std::fmt::Display for Type {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Type::I64 => write!(f, "i64"),
            Type::F64 => write!(f, "f64"),
            Type::Bool => write!(f, "bool"),
            Type::Char => write!(f, "char"),
            Type::Str => write!(f, "&str"),
            Type::String => write!(f, "String"),
            Type::Unit => write!(f, "()"),
            Type::Vec(t) => write!(f, "Vec<{}>", t),
            Type::Tuple(ts) => {
                write!(f, "(")?;
                for (i, t) in ts.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{}", t)?;
                }
                write!(f, ")")
            }
            Type::Unknown => write!(f, "_"),
        }
    }
}
