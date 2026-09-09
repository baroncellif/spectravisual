# Internal release process

## Versioning

Use semantic versions with the internal suffix while the product remains
private: `vMAJOR.MINOR.PATCH-internal`.

## Release checklist

1. `main` is clean and CI passes on macOS and Ubuntu.
2. The changelog is updated with user-visible changes and known limitations.
3. A scientist completes the keyboard and dual-view acceptance protocol.
4. Build the release binary from the tagged commit; record platform and build
   dependencies.
5. Create a private GitHub release with the tag, release notes, and approved
   artifacts.

Never tag a release from a local working tree with uncommitted modifications.
