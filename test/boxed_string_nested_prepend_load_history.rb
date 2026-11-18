class BoxedStringSizedPeer
  def size = 91
end

module BoxedStringLateSize
  def size = super + 10
end

def show_size(value)
  p value.size
end

value = [+"abc", BoxedStringSizedPeer.new][0]
show_size(value)
1.times do
  class String
    prepend BoxedStringLateSize
  end
end
show_size(value)
class String
  def size = 8
end
show_size(value)
