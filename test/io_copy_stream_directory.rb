# IO.copy_stream raises the read's Errno when the source is a directory,
# whichever kind of destination it writes to, and still copies a file.
require 'stringio'
require 'tmpdir'

def copy(label)
  n = yield
  puts "#{label}: #{n}"
rescue SystemCallError => e
  puts "#{label}: #{e.class}: #{e.message}"
end

def copy_from(src, dst)
  IO.copy_stream(src, dst)
end

Dir.mktmpdir do |dir|
  path = File.join(dir, "src.txt")
  File.write(path, "hello")
  out = File.join(dir, "out.txt")

  copy("dir io to stdout") { IO.copy_stream(File.open(dir), $stdout) }
  copy("dir io to stringio") { IO.copy_stream(File.open(dir), StringIO.new) }
  copy("dir io to file") { File.open(out, "w") { |f| IO.copy_stream(File.open(dir), f) } }
  copy("dir path to path") { IO.copy_stream(dir, out) }
  copy("boxed dir io") { copy_from(File.open(dir), StringIO.new) }
  copy("boxed stringio") { copy_from(StringIO.new("abc"), StringIO.new) }

  sink = StringIO.new
  copy("file io to stringio") { IO.copy_stream(File.open(path), sink) }
  p sink.string
  copy("file path to path") { IO.copy_stream(path, out) }
  p File.read(out)
end
