# Array#push across typed-array variants.
# Previously push only dispatched for IntArray and StrArray; FloatArray
# and SymArray (and PtrArray) silently fell through to a no-op expression.

ints = [1, 2, 3]
ints.push(4)
ints.push(5)
puts ints.length          # 5
puts ints[3]              # 4
puts ints[4]              # 5

# Float values use non-integer fractional parts so Spinel's float-puts
# (which strips a trailing ".0") matches CRuby's output.
floats = [1.5, 2.5]
floats.push(3.5)
floats.push(4.25)
puts floats.length        # 4
puts floats[2]            # 3.5
puts floats[3]            # 4.25

strs = ["a", "b"]
strs.push("c")
strs.push("d")
puts strs.length          # 4
puts strs[2]              # c
puts strs[3]              # d

syms = [:x, :y]
syms.push(:z)
puts syms.length          # 3
puts syms[2]              # z

# << on every typed-array variant. Previously << did not dispatch
# for sym_array, so `syms << :z` silently fell through.
ints2 = [10]
ints2 << 20
ints2 << 30
puts ints2.length         # 3
puts ints2[2]             # 30

floats2 = [1.5]
floats2 << 2.5
puts floats2.length       # 2
puts floats2[1]           # 2.5

strs2 = ["x"]
strs2 << "y"
puts strs2.length         # 2
puts strs2[1]             # y

syms2 = [:p]
syms2 << :q
puts syms2.length         # 2
puts syms2[1]             # q
