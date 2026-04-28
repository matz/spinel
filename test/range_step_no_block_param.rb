# When `Integer#step` is called with a do-block that omits its
# parameter, the generated C used a synthesized `_i` index without
# declaring it. Two paramless `step` blocks in the same function also
# caused a redefinition error. Wrap each in a scoped block and declare
# the synthesized variable locally.

sum = 0

# Single paramless step — `_i` was undeclared.
1.step(10, 1) do
  sum += 1
end

# Two paramless steps in the same scope — `_i` was redefined.
1.step(5, 1) do
  sum += 10
end
1.step(5, 1) do
  sum += 100
end

puts sum
