# Wayland protocol definitions

Vendored from the wlroots **wlr-protocols** project on 2026-09-23:
https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/tree/master/unstable

The XML files include their upstream copyright and permissive license notices.
Keep those notices in source and installed copies. CMake generates client/server
bindings with wayland-scanner; generated code is not checked into Git.

The compositor advertises layer-shell version 4 and foreign-toplevel-management
version 3. Clients must bind no higher than the advertised version.

Source SHA-256 checksums:

```text
87e0b9c837aecd6977f76f3c47d73088b7159871f5d979dc1840f6cadb5e2ed8  wlr-layer-shell-unstable-v1.xml
4ecc4588858e29fe680a33521e1f22bcf22071d66d1553fe96b4ddec03d591d2  wlr-foreign-toplevel-management-unstable-v1.xml
```
