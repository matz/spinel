# The argument expression runs on every receiver. Only an Array converts it
# to an Integer when dispatch reaches rotate or sample.
class Count
  def to_int
    p :to_int
    3
  end
end

def note(count)
  p :argument
  count
end

def rotate(receiver, count)
  p receiver.rotate(note(count))
rescue NoMethodError
  p :no_rotate
end

def sample(receiver, count)
  p receiver.sample(note(count)).sort
rescue NoMethodError
  p :no_sample
end

def plain_count
  p :plain_argument
  1
end

def rotate_plain(receiver)
  p receiver.rotate(plain_count)
rescue NoMethodError
  p :no_rotate
end

[[3, 1, 2], 1..3, [3, 1, 2].each, Time.at(0)..Time.at(2)].each do |receiver|
  rotate(receiver, Count.new)
  sample(receiver, Count.new)
  rotate_plain(receiver)
end
