# A String handed to IO#write, print, << or puts keeps the bytes after an
# embedded NUL when it reaches the write as a shared handle under
# --share-strings. The append below is to a value the analysis cannot
# follow; it marks the Strings it cannot follow as changed in place, and the
# ones the thread's block captures then reach the write as handles.
require "tmpdir"

o = Object.new
o.instance_variable_set(:@b, +"")
o.instance_variable_get(:@b) << "x"

payloads = [["6162006364"].pack("H*"), "plain"]
path = File.join(Dir.tmpdir, "spinel_io_write_shared_string_nul.txt")
f = File.open(path, "w")
t = Thread.new do
  payloads.each do |body|
    f.write(body)
    f.print(body)
    f << body
    f.puts(body)
  end
end
t.join
f.close
p File.binread(path)
File.delete(path)
