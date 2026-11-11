# Etc.getpwuid and Etc.getpwnam answer an Etc::Passwd of the user database:
# the calling user by default, root by name, and a missing user raises
# ArgumentError as CRuby does.
require "etc"

u = Etc.getpwuid
p u.class
p u.uid == Process.uid
p u.gid == Process.gid
p u.name.is_a?(String) && !u.name.empty?
p u.passwd.is_a?(String)
p u.gecos.is_a?(String)
p File.directory?(u.dir)
p u.shell.is_a?(String)

# the same user by name, and by an explicit uid
p Etc.getpwnam(u.name).uid == u.uid
p Etc.getpwuid(u.uid).name == u.name

# root
p Etc.getpwnam("root").uid
p Etc.getpwuid(0).name

# a user that is not there
begin
  Etc.getpwnam("no-such-user-xyz")
rescue ArgumentError => e
  p e.message
end
begin
  Etc.getpwuid(1073741824)
rescue ArgumentError => e
  p e.message
end
