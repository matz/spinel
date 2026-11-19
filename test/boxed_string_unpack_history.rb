class UnpackPeer
  def unpack1(format)
    [:peer, format]
  end
end

def unpack_value(value)
  value.unpack1("C")
end

value = [+"abc", UnpackPeer.new][0]
p unpack_value(value)
class String
  def unpack1(format = "default")
    [:first, format]
  end
end
p unpack_value(value)
class String
  undef_method :unpack1
end
begin
  p unpack_value(value)
rescue NoMethodError => e
  p e.name
end
class String
  def unpack1(format)
    [:restored, format]
  end
end
p unpack_value(value)
p unpack_value(UnpackPeer.new)
