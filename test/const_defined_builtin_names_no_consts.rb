# A program with no constant of its own still asks for a builtin's name
# at run time
n = ["Str", "uct"].join
p Object.const_defined?(n)
p Object.const_defined?(["Thr", "ead"].join)
p Object.const_defined?(["No", "Such"].join)
