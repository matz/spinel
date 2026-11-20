# File.expand_path("~user") is the home of that user, as CRuby answers it
# from the passwd database, with or without a base directory; an unknown
# user is an ArgumentError.
require "etc"
root = Etc.getpwnam("root").dir
p File.expand_path("~root") == root
p File.expand_path("~root/x/../y") == File.join(root, "y")
p File.expand_path("~root", "/tmp") == root
p File.expand_path("~root/", "/tmp") == root
me = Etc.getpwuid(Process.uid)
p File.expand_path("~#{me.name}") == me.dir
p File.expand_path("~") == File.expand_path("~#{me.name}") || ENV["HOME"] != me.dir
begin
  File.expand_path("~no_such_user_here")
rescue ArgumentError => e
  p e.message
end
begin
  File.expand_path("~no_such_user_here/x")
rescue ArgumentError => e
  p e.message
end
p File.expand_path("a~root", "/tmp")
begin
  File.expand_path("~root~", "/tmp")
rescue ArgumentError => e
  p e.message
end
