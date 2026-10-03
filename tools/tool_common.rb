# Shared helpers for the in-tree spinel tools (spinel-doctor / spinel-reduce /
# spinel-flatten). Compiled into each tool by `require_relative`; written in the
# spinel subset so the tools themselves compile with spinel.

# Exit status of the most recent sh() call.
$sh_status = 0

# Run a shell command, capturing stdout+stderr together, and record the exit
# status in $sh_status. Returns the captured output.
def sh(cmd)
  out = `#{cmd} 2>&1`
  $sh_status = $?
  out
end

# `s` as one shell word.
def shell_word(s)
  "'" + s.gsub("'", "'\\''") + "'"
end

# Locate the spinel compiler binary. Resolution order: $SPINEL (explicit path),
# $SPINEL_DIR/spinel, then `spinel` on PATH. Exits `miss` (2, unless the
# tool's contract gives that status another meaning) if none resolve.
def find_spinel(miss = 2)
  e = ENV["SPINEL"]
  return e if e && e.length > 0 && File.exist?(e)
  d = ENV["SPINEL_DIR"]
  if d && d.length > 0
    p = d + "/spinel"
    return p if File.exist?(p)
  end
  w = `command -v spinel 2>/dev/null`.strip
  return w if w.length > 0
  $stderr.puts "spinel-tool: cannot find the spinel compiler."
  $stderr.puts "  set SPINEL=/path/to/spinel, or SPINEL_DIR, or put spinel on PATH."
  exit(miss)
end

# Is `name` an executable on PATH?
def have_cmd(name)
  `command -v #{name} 2>/dev/null`.strip.length > 0
end

# The last path component of `path`.
def base_name(path)
  parts = path.split("/")
  parts[parts.length - 1]
end

# The directory of `path` ("." when there is no slash).
def dir_name(path)
  parts = path.split("/")
  return "." if parts.length < 2
  parts[0, parts.length - 1].join("/")
end

# A scratch path under TMPDIR (or /tmp), tagged by the tool and source name so
# concurrent runs on different sources don't collide.
def tmp_path(tag, src, ext)
  d = ENV["TMPDIR"]
  d = "/tmp" if !d || d.length == 0
  d + "/spinel-" + tag + "-" + base_name(src) + ext
end

# Print to stderr and exit with `code`.
def die(msg, code)
  $stderr.puts msg
  exit(code)
end
