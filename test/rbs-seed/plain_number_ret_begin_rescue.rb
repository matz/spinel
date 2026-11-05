# spinel: rbs-seed-run
# A method signed `-> Integer` or `-> Float` whose begin/rescue body answers
# a Mutex#synchronize block's value, which is boxed. The tail narrows the
# box into the plain number the signature declares. It took the nilable
# form, an sp_oint, and returned it through the sp_int signature, so the C
# did not build. An `Integer?` signature keeps its nil, as in
# nilable_ret_begin_rescue.
class BeginRetCounter
  def initialize
    @mutex = Mutex.new
  end

  def size_of(text)
    @mutex.synchronize { text.bytesize }
  rescue IOError => e
    raise ArgumentError, e.message
  end

  def double(x)
    @mutex.synchronize { x * 2 }
  rescue IOError => e
    raise ArgumentError, e.message
  end

  def size_or_nil(text)
    @mutex.synchronize { text.empty? ? nil : text.bytesize }
  rescue IOError => e
    raise ArgumentError, e.message
  end
end

c = BeginRetCounter.new
p c.size_of("abc")
p c.size_of("de") + 1
p c.double(1.5)
p c.double(2.0) * 2
p c.size_or_nil("xyz")
p c.size_or_nil("")
