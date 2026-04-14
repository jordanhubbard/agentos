use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

use crate::interpreter::Value;
use crate::error::{CrustError, CrustResult, Span};

pub type ValueRef = Rc<RefCell<Value>>;

#[derive(Debug, Clone)]
pub struct Env {
    frames: Vec<HashMap<String, ValueRef>>,
}

impl Env {
    pub fn new() -> Self {
        Env { frames: vec![HashMap::new()] }
    }

    pub fn push_scope(&mut self) {
        self.frames.push(HashMap::new());
    }

    pub fn pop_scope(&mut self) {
        if self.frames.len() > 1 {
            self.frames.pop();
        }
    }

    pub fn define(&mut self, name: &str, val: ValueRef) {
        if let Some(frame) = self.frames.last_mut() {
            frame.insert(name.to_string(), val);
        }
    }

    pub fn get(&self, name: &str, span: &Span) -> CrustResult<ValueRef> {
        for frame in self.frames.iter().rev() {
            if let Some(v) = frame.get(name) {
                return Ok(Rc::clone(v));
            }
        }
        Err(CrustError::RuntimeError {
            msg: format!("undefined variable '{}'", name),
            span: Some(span.clone()),
        })
    }

    pub fn set(&mut self, name: &str, val: ValueRef, span: &Span) -> CrustResult<()> {
        for frame in self.frames.iter_mut().rev() {
            if let Some(existing) = frame.get_mut(name) {
                *existing = val;
                return Ok(());
            }
        }
        Err(CrustError::RuntimeError {
            msg: format!("cannot assign to undeclared variable '{}'", name),
            span: Some(span.clone()),
        })
    }

    /// Assign into the inner value via RefCell (for field/index mutations)
    pub fn set_inner(&mut self, name: &str, val: Value, span: &Span) -> CrustResult<()> {
        for frame in self.frames.iter().rev() {
            if let Some(existing) = frame.get(name) {
                *existing.borrow_mut() = val;
                return Ok(());
            }
        }
        Err(CrustError::RuntimeError {
            msg: format!("cannot assign to undeclared variable '{}'", name),
            span: Some(span.clone()),
        })
    }
}
