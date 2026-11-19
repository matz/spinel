# frozen_string_literal: true

class Yielder
  def yield(*values, **keywords)
    [values, keywords]
  end
end

def choose(values, index)
  values[index]
end

accept = [proc { |value = :default, **keywords| [value, keywords] }, Yielder.new]
pack = [proc { |first, second| [first, second] }, Yielder.new]
hash = [{ marker: 1 }]
array = [[1, 2]]
empty = {}
2.times do |index|
  p choose(accept, index).yield(*hash)
  p choose(pack, index).yield(*array)
  p choose(pack, index).yield(*array, **empty)
  p choose(pack, index).yield(array[0], **empty)
end
