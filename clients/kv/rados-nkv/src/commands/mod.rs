//! Subcommand implementations (bead spdk-jhk.7.4 onward).
//!
//! The datapath commands (store/get) live in `main.rs` directly for now; the
//! control-plane `ns` subtree (JSON-RPC) lives here, along with `list` and the
//! `store`/`get` `-o` option parsing (bead spdk-jhk.7.5).

pub mod list;
pub mod ns;
pub mod options;
