# IO#read with no length raises EISDIR on a handle opened on a directory,
# and still reads a file to the end.
require 'tmpdir'

Dir.mktmpdir do |dir|
  begin
    File.open(dir) { |f| f.read }
    puts "no error"
  rescue Errno::EISDIR => e
    puts e.message.sub(dir, "DIR")
  end

  path = File.join(dir, "a.txt")
  File.write(path, "abc\ndef\n")
  File.open(path) { |f| p f.read }
  File.open(path) { |f| f.read(4); p f.read; p f.read }
end
