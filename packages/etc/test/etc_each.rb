# Etc.passwd and Etc.group with a block walk the whole user and group
# databases, one Etc::Passwd or Etc::Group at a time, and the calling user
# and group are among the entries. Each call starts over from the first
# entry.
require "etc"

names = []
Etc.passwd { |u| names << u.name }
p names.include?(Etc.getpwuid.name)
p names.include?("root")
all_passwd = true
Etc.passwd { |u| all_passwd &&= u.is_a?(Etc::Passwd) && u.uid.is_a?(Integer) }
p all_passwd

again = []
Etc.passwd { |u| again << u.name }
p again == names

groups = []
Etc.group { |g| groups << g.name }
p groups.include?(Etc.getgrgid.name)
all_group = true
Etc.group { |g| all_group &&= g.is_a?(Etc::Group) && g.gid.is_a?(Integer) && g.mem.is_a?(Array) }
p all_group

# the block's value is not the answer: both answer nil, as CRuby's do
p Etc.passwd { |u| 1 }
p Etc.group { |g| 1 }
