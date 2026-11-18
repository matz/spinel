# spinel: gc-stress
# The path and the mode that IO#reopen gives one of the three standard
# streams are roots: their handles are static, so no scan marks them.
def name(s) = s + ""
def redirect
  $stderr.reopen(name(File::NULL), name("w"))
end
redirect
junk = Array.new(50) { |k| "junk #{k}" }
$stdout.puts $stderr.path
begin
  $stderr.write("hidden\n")
  $stdout.puts "ok #{junk.size}"
rescue IOError => e
  $stdout.puts "IOError: #{e.message}"
end
