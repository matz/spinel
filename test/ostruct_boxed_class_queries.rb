# An OpenStruct's class remains a readable Class value when the receiver is
# boxed in an Array; kind queries distinguish it from other boxed values.
require "ostruct"

rows = []
o = OpenStruct.new
o["a"] = 1
rows << o
x = rows.first
p x.is_a?(OpenStruct)
p x.kind_of?(OpenStruct)
p x.instance_of?(OpenStruct)
p(OpenStruct === x)
p(case x when OpenStruct then "struct" else "other" end)
p x.class == OpenStruct
p x.class
p OpenStruct
p x.is_a?(Object)

[o, 7].each do |value|
  p value.is_a?(OpenStruct)
  p value.kind_of?(OpenStruct)
  p value.instance_of?(OpenStruct)
  p(OpenStruct === value)
  p(case value when OpenStruct then "struct" else "other" end)
  p value.class == OpenStruct
  p value.class
  p value.is_a?(Object)
end

# A statically typed receiver keeps the same answers.
p o.is_a?(OpenStruct)
p o.kind_of?(OpenStruct)
p o.instance_of?(OpenStruct)
p o.is_a?(Object)
p o.instance_of?(Object)
p o.class == OpenStruct
p o.class
