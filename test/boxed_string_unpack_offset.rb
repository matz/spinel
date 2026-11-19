class UnpackOffsetPeer
  def unpack1(format, offset: 0)
    [:peer, format, offset]
  end
end

module UnpackOffsetPatch
  def unpack1(format, offset: 0)
    [:prepend, super]
  end
end

def unpack_at_offset(value)
  value.unpack1("C", offset: 1)
end

def capture_unpack(value)
  value.method(:unpack1)
end

before = capture_unpack(+"abc")
p unpack_at_offset(+"abc")
class String
  prepend UnpackOffsetPatch if ARGV.empty?
end
p unpack_at_offset(+"abc"), unpack_at_offset(UnpackOffsetPeer.new)
p before.call("C", offset: 1), before.arity
after = capture_unpack(+"abc")
p after.call("C", offset: 1), after.arity
