# A user class with a method whose name overlaps an Array method (e.g.
# `def sample`, `def first`) used to compile to the Array dispatch even
# when the receiver wasn't an array. `array_c_prefix` falls back to
# `IntArray`, so e.g. `m.sample` on `sp_Mixer *` emitted
# `sp_IntArray_get(m, rand() % sp_IntArray_length(m))` and gcc rejected
# the pointer-type mismatch.

class Mixer
  def sample
    42
  end
end

m = Mixer.new
puts m.sample
