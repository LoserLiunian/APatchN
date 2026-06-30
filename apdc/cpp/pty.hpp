#pragma once
// Pseudo-terminal allocation for the root shell. Mirrors ../apd/src/pty.rs
// (itself based on Magisk's pts.cpp). Throws on failure.
namespace apd::pty {
void prepare_pty();
} // namespace apd::pty
