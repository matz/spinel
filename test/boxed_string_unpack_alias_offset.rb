def call_saved_unpack
  "AB".saved_unpack1("C", offset: 1)
end

class String
  alias saved_unpack1 unpack1

  def unpack1(format, offset: 0)
    [:override, format, offset]
  end
end

p call_saved_unpack
p "AB".saved_unpack1("C")
p "AB".saved_unpack1("C", offset: 1)
p "AB".unpack1("C", offset: 1)

class String
  def saved_unpack1(format, offset: -1)
    [:new_alias, format, offset]
  end
end

p call_saved_unpack
