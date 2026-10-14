## MODIFIED Requirements

### Requirement: Self-healing and start-at-login
A managed daemon that exits unexpectedly SHALL be restarted, and the whole supervised set SHALL come
up automatically at user login without a manual command. These lifecycle guarantees SHALL be
provided through the platform service manager: on macOS `launchd` (per-daemon LaunchAgents with
`KeepAlive` and `RunAtLoad`, plus a supervisor LaunchAgent that runs reconciliation at load and on a
recurring interval); on Linux `systemd --user` (per-daemon `<label>.service` units with
`Restart=always`, no start limit and `WantedBy=default.target`, plus a oneshot supervisor service
driven by a `<label>.timer` with `OnActiveSec=0` and `OnUnitActiveSec=<interval>`). Labels are the
same on both platforms.

#### Scenario: Crashed daemon is restarted
- **WHEN** a managed daemon process for a tracked repo terminates unexpectedly
- **THEN** it is restarted and resumes serving that repo

#### Scenario: Supervised set starts at login
- **WHEN** the user logs in after the supervisor has been installed
- **THEN** reconciliation runs and a daemon is present for each tracked repo without a manual command

#### Scenario: Linux units parse
- **WHEN** the per-repo and supervisor units are rendered for a repo path containing spaces, `%` and `$`
- **THEN** `systemd-analyze verify` accepts each unit and graphd receives the path as one literal argument

### Requirement: Supervisor management command
The `cgraph` CLI SHALL provide a `daemon` subcommand family to manage the supervisor on macOS and
Linux: `install` (register and start the supervisor and reconcile once), `sync` (reconcile the
daemon set against discovery now), `status` (report each tracked repo and whether its daemon is
live), and `uninstall` (stop and remove the supervisor and all managed daemons). `install` and
`uninstall` SHALL be reversible — `uninstall` after `install` SHALL leave no managed service
definition (LaunchAgent plist, or systemd unit and timer) behind. On Linux, a definition outside the
user manager's unit directory SHALL only be deleted, never stopped by label. The supervisor SHALL
never pull, commit, or otherwise mutate any tracked repo's contents; it only reads the working tree
to build the graph.

#### Scenario: Install then uninstall leaves no residue
- **WHEN** `cgraph daemon install` is run and later `cgraph daemon uninstall` is run
- **THEN** no supervisor or per-repo managed service definition remains and no managed daemon is running

#### Scenario: Status reports liveness per tracked repo
- **WHEN** `cgraph daemon status` is run with some tracked repos served and others not
- **THEN** it lists each tracked repo and reports for each whether a live daemon is currently serving
  it

#### Scenario: Supervisor never mutates a repo
- **WHEN** the supervisor manages a tracked repo across many reconciliation cycles and edits
- **THEN** it performs no git operation and makes no change to the repo's tracked files
