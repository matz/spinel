class StoredNotices
  def initialize
    @items = []
  end
  def push(value)
    @items.push(value)
    nil
  end
end
class NoticeCopy
  attr_accessor :notice
  def initialize(text)
    @notice = text
  end
  def retain(store)
    store.push(@notice)
    nil
  end
  def copy
    other = NoticeCopy.new(nil)
    other.notice = @notice
    other
  end
  def assign(other)
    other.notice = @notice
    nil
  end
end
store = StoredNotices.new
store.push(17)
first = NoticeCopy.new(+"notice")
first.retain(store)
second = first.copy
first.notice << "!"
p first.notice, second.notice
third = NoticeCopy.new(nil)
p first.assign(third)
first.notice << "?"
p third.notice
p NoticeCopy.new(nil).copy.notice
second.freeze
begin
  first.assign(second)
rescue FrozenError
  p :frozen_writer
end
