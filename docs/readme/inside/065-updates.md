- **Updates**: the App Store's Updates page downloads a newer NovaOS from
  an update channel signed with the release key (Ed25519), and the next
  restart starts it, going back to the old one if it does not start
  ([docs/updates.md](docs/updates.md)).
  Every green build of `main` is also an update, on its own channel.
