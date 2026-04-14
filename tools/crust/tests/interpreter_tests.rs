use std::process::Command;

fn crust_run(source: &str) -> Result<String, String> {
    // Write to a temp file and run (use thread id to avoid collisions in parallel tests)
    let tid = format!("{:?}", std::thread::current().id())
        .replace("ThreadId(", "").replace(")", "");
    let path = std::env::temp_dir().join(format!("crust_test_{}_{}.crs", std::process::id(), tid));
    std::fs::write(&path, source).unwrap();
    let output = Command::new(env!("CARGO_BIN_EXE_crust"))
        .arg("run")
        .arg(&path)
        .output()
        .map_err(|e| e.to_string())?;
    std::fs::remove_file(&path).ok();
    if output.status.success() {
        Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
    } else {
        Err(String::from_utf8_lossy(&output.stderr).trim().to_string())
    }
}

#[test]
fn test_hello_world() {
    let src = r#"
fn main() {
    println!("Hello, world!");
}
"#;
    assert_eq!(crust_run(src).unwrap(), "Hello, world!");
}

#[test]
fn test_arithmetic() {
    let src = r#"
fn main() {
    let x = 10 + 5;
    let y = x * 2;
    let z = y - 3;
    println!("{}", z);
}
"#;
    assert_eq!(crust_run(src).unwrap(), "27");
}

#[test]
fn test_if_else() {
    let src = r#"
fn main() {
    let x = 10;
    if x > 5 {
        println!("big");
    } else {
        println!("small");
    }
}
"#;
    assert_eq!(crust_run(src).unwrap(), "big");
}

#[test]
fn test_while_loop() {
    let src = r#"
fn main() {
    let mut i = 0;
    let mut sum = 0;
    while i < 5 {
        sum = sum + i;
        i = i + 1;
    }
    println!("{}", sum);
}
"#;
    assert_eq!(crust_run(src).unwrap(), "10");
}

#[test]
fn test_for_loop() {
    let src = r#"
fn main() {
    let mut sum = 0;
    for i in 0..5 {
        sum = sum + i;
    }
    println!("{}", sum);
}
"#;
    assert_eq!(crust_run(src).unwrap(), "10");
}

#[test]
fn test_for_vec() {
    let src = r#"
fn main() {
    let mut sum = 0;
    let nums = vec![1, 2, 3, 4, 5];
    for n in nums {
        sum = sum + n;
    }
    println!("{}", sum);
}
"#;
    assert_eq!(crust_run(src).unwrap(), "15");
}

#[test]
fn test_functions() {
    let src = r#"
fn add(a: i64, b: i64) -> i64 {
    return a + b;
}

fn main() {
    let result = add(3, 4);
    println!("{}", result);
}
"#;
    assert_eq!(crust_run(src).unwrap(), "7");
}

#[test]
fn test_recursion() {
    let src = r#"
fn fib(n: i64) -> i64 {
    if n <= 1 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

fn main() {
    println!("{}", fib(10));
}
"#;
    assert_eq!(crust_run(src).unwrap(), "55");
}

#[test]
fn test_string_ops() {
    let src = r#"
fn main() {
    let s = "hello";
    println!("{}", s.to_uppercase());
    println!("{}", s.len());
}
"#;
    assert_eq!(crust_run(src).unwrap(), "HELLO\n5");
}

#[test]
fn test_vec_ops() {
    let src = r#"
fn main() {
    let mut v = vec![3, 1, 2];
    v.push(4);
    println!("{}", v.len());
    v.sort();
    println!("{}", v.get(0));
}
"#;
    assert_eq!(crust_run(src).unwrap(), "4\n1");
}

#[test]
fn test_format_macro() {
    let src = r#"
fn main() {
    let s = format!("Hello, {}!", "world");
    println!("{}", s);
}
"#;
    assert_eq!(crust_run(src).unwrap(), "Hello, world!");
}

#[test]
fn test_compound_assign() {
    let src = r#"
fn main() {
    let mut x = 10;
    x += 5;
    x -= 2;
    x *= 3;
    println!("{}", x);
}
"#;
    assert_eq!(crust_run(src).unwrap(), "39");
}

#[test]
fn test_boolean_logic() {
    let src = r#"
fn main() {
    let a = true;
    let b = false;
    println!("{}", a && b);
    println!("{}", a || b);
    println!("{}", !a);
}
"#;
    assert_eq!(crust_run(src).unwrap(), "false\ntrue\nfalse");
}

#[test]
fn test_break_continue() {
    let src = r#"
fn main() {
    let mut sum = 0;
    for i in 0..10 {
        if i == 5 {
            break;
        }
        if i % 2 == 0 {
            continue;
        }
        sum = sum + i;
    }
    println!("{}", sum);
}
"#;
    // odd numbers from 1..5 = 1+3 = 4
    assert_eq!(crust_run(src).unwrap(), "4");
}

#[test]
fn test_nested_functions() {
    let src = r#"
fn square(x: i64) -> i64 {
    return x * x;
}

fn sum_of_squares(n: i64) -> i64 {
    let mut total = 0;
    let mut i = 1;
    while i <= n {
        total = total + square(i);
        i = i + 1;
    }
    return total;
}

fn main() {
    println!("{}", sum_of_squares(4));
}
"#;
    // 1 + 4 + 9 + 16 = 30
    assert_eq!(crust_run(src).unwrap(), "30");
}

#[test]
fn test_main_program() {
    let src = r#"
fn main() {
    let x = 42;
    let name = "world";
    println!("Hello, {}! The answer is {}", name, x);

    let mut numbers = vec![1, 2, 3, 4, 5];
    let mut sum = 0;
    for n in numbers {
        sum = sum + n;
    }
    println!("Sum: {}", sum);

    if sum > 10 {
        println!("Big sum!");
    } else {
        println!("Small sum");
    }

    let mut i = 0;
    while i < 3 {
        println!("i = {}", i);
        i = i + 1;
    }
}
"#;
    let expected = "Hello, world! The answer is 42\nSum: 15\nBig sum!\ni = 0\ni = 1\ni = 2";
    assert_eq!(crust_run(src).unwrap(), expected);
}
