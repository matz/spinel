# PTY.spawn given Strings that are the shared handle (a name aliases each
# and changes it in place): an env key and value, and chdir, read as the
# Strings they hold, where they were refused as no String.
require "pty"
require "tmpdir"
def drain(r)
  out = ""
  begin
    loop { out = out + r.readpartial(1024) }
  rescue Errno::EIO, EOFError
    nil
  end
  out.delete("\r")
end
Dir.mktmpdir("pty_handle") do |dir|
  d = +dir
  e = d
  e << ""
  word = +"hello"
  w2 = word
  w2 << "!"
  key = +"PTY_HANDLE_VAR"
  k2 = key
  k2 << ""
  r, w, pid = PTY.spawn({ key => word }, "echo $PTY_HANDLE_VAR; pwd", chdir: d)
  out = drain(r)
  Process.waitpid2(pid)
  r.close
  w.close
  p out.lines.map(&:chomp) == ["hello!", File.realpath(dir)]
end
