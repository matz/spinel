# Etc.getgrgid and Etc.getgrnam answer an Etc::Group of the group database:
# the calling group by default, the member list as an Array of names, and
# a missing group raises ArgumentError as CRuby does.
require "etc"

g = Etc.getgrgid
p g.class
p g.gid == Process.gid
p g.name.is_a?(String) && !g.name.empty?
p g.passwd.is_a?(String)
p g.mem.is_a?(Array)
p g.mem.all? { |m| m.is_a?(String) && !m.empty? }

# the same group by name, and by an explicit gid
p Etc.getgrnam(g.name).gid == g.gid
p Etc.getgrgid(g.gid).name == g.name

# gid 0 is there on every Unix
p Etc.getgrgid(0).gid

# a group that is not there
begin
  Etc.getgrnam("no-such-group-xyz")
rescue ArgumentError => e
  p e.message
end
begin
  Etc.getgrgid(1073741824)
rescue ArgumentError => e
  p e.message
end
