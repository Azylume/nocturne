# One content-addressed media library per machine

Compositions refer to media by content hash, not by file path, and every machine keeps one shared media library indexed by hash. Moving or renaming a file, or opening a composition on another machine, never breaks a clip, and node sync copies only the hashes a machine lacks.

Path-based references were rejected after real pain moving Resolume compositions between machines (Mac to Windows), where every clip went missing.
