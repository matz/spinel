# spinel: share
# A spawn's filename redirection given a String that is the shared handle
# (a name aliases it and changes it in place) opens that file: the
# generated redirect test took a boxed String by its tag alone.
require "tmpdir"
Dir.mktmpdir do |d|
  path = +"#{d}/out.txt"
  q = path
  q << ""
  opts = { out: [path, 1][0] }
  pid = Process.spawn("/bin/echo", "hi", **opts)
  Process.wait(pid)
  p File.read(path)
end
