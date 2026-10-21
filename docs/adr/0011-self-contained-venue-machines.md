# Every venue machine is self-contained; there is no main engine

Supersedes ADR 0006.

Each venue has its own show machine with its own composition, Media Library, beat clock and outputs, and it runs alone. A Director (the UI on the operator's machine) connects to one venue at a time over Tailscale and edits it directly. No machine is main, and no venue depends on the Director or the internet during a gig. Media lives in a private Cloudflare R2 bucket by content hash. Each venue pulls only what its own show uses, and only outside its gig window.

We chose this over full replication with a promotable main (0006) because the goal changed. 0006 aimed at one show spread across several machines in one room. Now the goal is several independent gigs on the same night, run remotely. Copying every venue's media to every machine would waste bandwidth and disk, and takeover by a promoted main has nothing to protect when each venue already keeps playing without its UI.

The cost: several machines sharing one show at one venue is no longer designed in. If a venue ever needs two machines, that comes back as its own decision. Node Sync stays only as the follower the laptop UI uses for local previews.
