# A String a block parameter holds is stored into a Hash under one Symbol
# key, and the Hash is mutated through another key that never holds a
# String (`options[:dirs] << dir`, an Array). The mutation is no String's,
# so the stored String stays a copy and each compiles and answers as CRuby,
# rather than being refused as a String mutated through the container.
def each_opt
  yield "a" + ""
  yield "/x"
end

# a literal with the Array under :dirs, and other keys written with ||=
options = { dirs: [] }
each_opt { |v| options[:name] = v }
each_opt { |v| options[:dirs] << v }
options[:width] ||= 72
options[:all] ||= options[:name]
p options

# through fetch, with the Hash built by a method
def defaults
  o = {}
  o[:dirs] = []
  o[:all] = false
  return o
end

def parse
  opts = defaults
  each_opt { |v| opts[:name] = v }
  each_opt { |v| opts.fetch(:dirs) << v.upcase }
  opts
end
p parse

