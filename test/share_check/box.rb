# A new String passed into a parameter other names share is boxed as a new
# handle (sp_box_str_new), so the append the method makes through the
# parameter reaches it: boxed as its bytes, it reached only the box's copy.
def append(a = nil, *rest, last)
  a << "!" if a.is_a?(String)
  [rest.size, last]
end
s = +"seed"
p append(s, 1)
p s
p append(+"fresh", *[1, 2], 3)
