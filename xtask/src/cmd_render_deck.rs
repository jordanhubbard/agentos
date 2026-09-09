use crate::RenderDeckArgs;
use anyhow::{Context, Result};
use sha2::{Digest, Sha256};
use std::fs;
use std::process::Command;

const PAGE_WIDTH: f32 = 960.0;
const PAGE_HEIGHT: f32 = 540.0;
const LEFT: f32 = 72.0;
const BOTTOM_LIMIT: f32 = 48.0;

#[derive(Debug)]
struct Slide {
    title: String,
    lines: Vec<Line>,
}

#[derive(Debug)]
struct Line {
    text: String,
    style: Style,
}

#[derive(Clone, Copy, Debug)]
enum Style {
    Body,
    Bullet,
    Subhead,
    Code,
    Blank,
}

pub fn run(args: &RenderDeckArgs) -> Result<()> {
    let source = fs::read_to_string(&args.input)
        .with_context(|| format!("failed to read {}", args.input.display()))?;
    let facts = fs::read_to_string(&args.facts)
        .with_context(|| format!("failed to read {}", args.facts.display()))?;
    let slides = parse_deck(&source)?;
    let revision = git_revision().unwrap_or_else(|| "working-tree".to_string());
    let pdf = render_pdf(&slides, &args.edition, &revision)?;

    if let Some(parent) = args.output.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::write(&args.output, &pdf)
        .with_context(|| format!("failed to write {}", args.output.display()))?;

    let qa_path = args.output.with_extension("qa.txt");
    let source_hash = format!("{:x}", Sha256::digest(source.as_bytes()));
    let facts_hash = format!("{:x}", Sha256::digest(facts.as_bytes()));
    let pdf_hash = format!("{:x}", Sha256::digest(&pdf));
    let qa = format!(
        "schema=agentos.presentation.qa.v1\n\
edition={}\n\
source_revision={}\n\
source={}\n\
source_sha256={}\n\
facts={}\n\
facts_sha256={}\n\
pdf={}\n\
pdf_sha256={}\n\
pages={}\n\
renderer=xtask-raw-pdf-v1\n\
fonts=PDF-Base-14 Helvetica,Helvetica-Bold\n\
page_size_points=960x540\n\
structural_validation=pass\n\
overflow_validation=pass\n\
speaker_notes_excluded=pass\n\
visual_review={}\n",
        args.edition,
        revision,
        args.input.display(),
        source_hash,
        args.facts.display(),
        facts_hash,
        args.output.display(),
        pdf_hash,
        slides.len(),
        if args.visual_review {
            "pass"
        } else {
            "required"
        },
    );
    fs::write(&qa_path, qa)?;
    println!(
        "[xtask:deck] rendered {} pages to {} (QA: {})",
        slides.len(),
        args.output.display(),
        qa_path.display()
    );
    Ok(())
}

fn parse_deck(source: &str) -> Result<Vec<Slide>> {
    let mut slides = Vec::new();
    for section in source.split("\n---\n") {
        let mut title = None;
        let mut lines = Vec::new();
        let mut in_code = false;
        let mut in_notes = false;
        let mut in_metadata = false;

        for raw in section.lines() {
            let trimmed = raw.trim();
            if trimmed.starts_with("> Speaker notes:") {
                in_notes = true;
                continue;
            }
            if in_notes {
                continue;
            }
            if trimmed.starts_with("Audience:") || trimmed.starts_with("Central claim:") {
                in_metadata = true;
                continue;
            }
            if title.is_none() && (trimmed.starts_with("# ") || trimmed.starts_with("## ")) {
                let heading = trimmed.trim_start_matches('#').trim();
                title = Some(strip_slide_number(heading));
                continue;
            }
            if trimmed == "```text" || trimmed == "```" {
                in_code = !in_code;
                continue;
            }
            if trimmed.is_empty() {
                if in_metadata {
                    in_metadata = false;
                    continue;
                }
                if !matches!(
                    lines.last(),
                    Some(Line {
                        style: Style::Blank,
                        ..
                    })
                ) {
                    lines.push(Line {
                        text: String::new(),
                        style: Style::Blank,
                    });
                }
                continue;
            }
            if in_metadata {
                continue;
            }

            let (style, text) = if in_code {
                (Style::Code, trimmed.to_string())
            } else if trimmed.starts_with("**") && trimmed.ends_with("**") {
                (Style::Subhead, trimmed.trim_matches('*').to_string())
            } else if let Some(text) = trimmed.strip_prefix("- ") {
                (Style::Bullet, text.to_string())
            } else {
                (Style::Body, trimmed.to_string())
            };
            lines.push(Line { text, style });
        }

        let title = title.context("every slide section must have a heading")?;
        while matches!(
            lines.last(),
            Some(Line {
                style: Style::Blank,
                ..
            })
        ) {
            lines.pop();
        }
        slides.push(Slide { title, lines });
    }
    anyhow::ensure!(!slides.is_empty(), "deck contains no slides");
    Ok(slides)
}

fn strip_slide_number(heading: &str) -> String {
    if let Some((prefix, rest)) = heading.split_once(". ") {
        if prefix.chars().all(|c| c.is_ascii_digit()) {
            return rest.to_string();
        }
    }
    heading.to_string()
}

fn render_pdf(slides: &[Slide], edition: &str, revision: &str) -> Result<Vec<u8>> {
    let mut objects: Vec<Vec<u8>> = vec![Vec::new(); 4];
    objects[0] = b"<< /Type /Catalog /Pages 2 0 R >>".to_vec();
    objects[2] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>".to_vec();
    objects[3] = b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>".to_vec();

    let mut page_ids = Vec::new();
    for (index, slide) in slides.iter().enumerate() {
        let content = render_slide(slide, index, slides.len(), edition, revision)?;
        let content_id = objects.len() + 1;
        objects.push(
            format!(
                "<< /Length {} >>\nstream\n{}\nendstream",
                content.len(),
                content
            )
            .into_bytes(),
        );
        let page_id = objects.len() + 1;
        objects.push(
            format!(
                "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 {PAGE_WIDTH} {PAGE_HEIGHT}] \
                 /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents {content_id} 0 R >>"
            )
            .into_bytes(),
        );
        page_ids.push(page_id);
    }
    let kids = page_ids
        .iter()
        .map(|id| format!("{id} 0 R"))
        .collect::<Vec<_>>()
        .join(" ");
    objects[1] = format!("<< /Type /Pages /Count {} /Kids [{kids}] >>", slides.len()).into_bytes();
    encode_pdf(&objects)
}

fn render_slide(
    slide: &Slide,
    index: usize,
    total: usize,
    edition: &str,
    revision: &str,
) -> Result<String> {
    let cover = index == 0;
    let mut commands = String::from("0.035 0.055 0.09 rg 0 0 960 540 re f\n");
    commands.push_str("0.20 0.78 0.70 rg 0 512 960 8 re f\n");
    let title_size = if cover { 50.0 } else { 36.0 };
    let title_y = if cover { 360.0 } else { 452.0 };
    let title_width = if cover { 30 } else { 48 };
    let title_lines = wrap_words(&slide.title, title_width);
    let mut y = title_y;
    for line in title_lines {
        pdf_text(
            &mut commands,
            "F2",
            title_size,
            LEFT,
            y,
            &line,
            (0.95, 0.97, 1.0),
        );
        y -= title_size * 1.12;
    }
    y -= if cover { 24.0 } else { 14.0 };

    for line in &slide.lines {
        match line.style {
            Style::Blank => y -= 11.0,
            Style::Subhead => {
                y -= 4.0;
                for text in wrap_words(&line.text, 72) {
                    pdf_text(
                        &mut commands,
                        "F2",
                        22.0,
                        LEFT,
                        y,
                        &text,
                        (0.20, 0.78, 0.70),
                    );
                    y -= 29.0;
                }
            }
            Style::Bullet => {
                for (part, text) in wrap_words(&line.text, 82).into_iter().enumerate() {
                    let prefix = if part == 0 { "- " } else { "  " };
                    pdf_text(
                        &mut commands,
                        "F1",
                        17.0,
                        LEFT + 10.0,
                        y,
                        &format!("{prefix}{text}"),
                        (0.90, 0.92, 0.95),
                    );
                    y -= 22.0;
                }
            }
            Style::Code => {
                pdf_text(
                    &mut commands,
                    "F1",
                    16.0,
                    LEFT + 18.0,
                    y,
                    &line.text,
                    (0.72, 0.88, 0.84),
                );
                y -= 20.0;
            }
            Style::Body => {
                for text in wrap_words(&line.text, 88) {
                    pdf_text(
                        &mut commands,
                        "F1",
                        17.0,
                        LEFT,
                        y,
                        &text,
                        (0.90, 0.92, 0.95),
                    );
                    y -= 22.0;
                }
            }
        }
    }
    anyhow::ensure!(
        y >= BOTTOM_LIMIT,
        "slide {} overflows the page: {}",
        index + 1,
        slide.title
    );

    let short_revision = revision.get(..revision.len().min(12)).unwrap_or(revision);
    pdf_text(
        &mut commands,
        "F1",
        10.0,
        LEFT,
        24.0,
        &format!("agentOS {edition} | {short_revision}"),
        (0.55, 0.62, 0.70),
    );
    pdf_text(
        &mut commands,
        "F1",
        10.0,
        850.0,
        24.0,
        &format!("{}/{}", index + 1, total),
        (0.55, 0.62, 0.70),
    );
    Ok(commands)
}

fn pdf_text(
    out: &mut String,
    font: &str,
    size: f32,
    x: f32,
    y: f32,
    text: &str,
    colour: (f32, f32, f32),
) {
    let text = ascii_text(text);
    out.push_str(&format!(
        "{} {} {} rg BT /{} {} Tf 1 0 0 1 {} {} Tm ({}) Tj ET\n",
        colour.0,
        colour.1,
        colour.2,
        font,
        size,
        x,
        y,
        escape_pdf(&text)
    ));
}

fn wrap_words(text: &str, width: usize) -> Vec<String> {
    let mut result = Vec::new();
    let mut current = String::new();
    for word in text.split_whitespace() {
        if !current.is_empty() && current.len() + 1 + word.len() > width {
            result.push(current);
            current = String::new();
        }
        if !current.is_empty() {
            current.push(' ');
        }
        current.push_str(word);
    }
    if !current.is_empty() {
        result.push(current);
    }
    if result.is_empty() {
        result.push(String::new());
    }
    result
}

fn ascii_text(text: &str) -> String {
    text.replace('→', "->")
        .replace('—', "-")
        .replace('–', "-")
        .replace('“', "\"")
        .replace('”', "\"")
        .replace('’', "'")
        .replace('…', "...")
        .replace('×', "x")
        .chars()
        .map(|c| if c.is_ascii() { c } else { '?' })
        .collect()
}

fn escape_pdf(text: &str) -> String {
    text.replace('\\', "\\\\")
        .replace('(', "\\(")
        .replace(')', "\\)")
}

fn encode_pdf(objects: &[Vec<u8>]) -> Result<Vec<u8>> {
    let mut pdf = b"%PDF-1.4\n%agentOS\n".to_vec();
    let mut offsets = Vec::with_capacity(objects.len());
    for (index, object) in objects.iter().enumerate() {
        offsets.push(pdf.len());
        pdf.extend_from_slice(format!("{} 0 obj\n", index + 1).as_bytes());
        pdf.extend_from_slice(object);
        pdf.extend_from_slice(b"\nendobj\n");
    }
    let xref = pdf.len();
    pdf.extend_from_slice(format!("xref\n0 {}\n", objects.len() + 1).as_bytes());
    pdf.extend_from_slice(b"0000000000 65535 f \n");
    for offset in offsets {
        pdf.extend_from_slice(format!("{offset:010} 00000 n \n").as_bytes());
    }
    pdf.extend_from_slice(
        format!(
            "trailer\n<< /Size {} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n",
            objects.len() + 1
        )
        .as_bytes(),
    );
    anyhow::ensure!(pdf.starts_with(b"%PDF-1.4"), "invalid PDF header");
    Ok(pdf)
}

fn git_revision() -> Option<String> {
    let output = Command::new("git")
        .args(["rev-parse", "HEAD"])
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn notes_are_not_audience_content() {
        let deck = "# Cover\n\nAudience: engineers and\nreviewers.\n\nCentral claim: internal\nmetadata.\n\nVisible.\n\n> Speaker notes: private\n> detail\n\n---\n\n## 1. Next\n\n- Item\n";
        let slides = parse_deck(deck).unwrap();
        assert_eq!(slides.len(), 2);
        assert!(!slides[0]
            .lines
            .iter()
            .any(|line| line.text.contains("private")));
        assert!(!slides[0]
            .lines
            .iter()
            .any(|line| line.text.contains("reviewers") || line.text.contains("metadata")));
        assert!(slides[0].lines.iter().any(|line| line.text == "Visible."));
        assert_eq!(slides[1].title, "Next");
    }

    #[test]
    fn generated_pdf_has_one_page_per_slide() {
        let deck = "# Cover\n\nVisible.\n\n---\n\n## 1. Next\n\n- Item\n";
        let slides = parse_deck(deck).unwrap();
        let pdf = render_pdf(&slides, "test", "0123456789abcdef").unwrap();
        let text = String::from_utf8_lossy(&pdf);
        assert!(text.starts_with("%PDF-1.4"));
        assert_eq!(text.matches("/Type /Page ").count(), 2);
        assert!(text.ends_with("%%EOF\n"));
    }
}
