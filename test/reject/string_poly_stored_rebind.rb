# CRuby reference for retained mutation and shared += rebinding. These routes
# must refuse until share-by-default, not acquire new per-route sharing.
class StoredStrings
  def initialize
    @items = []
  end
  def push(value)
    @items.push(value)
    nil
  end
  def at(index) = @items[index]
end
def forward_store(store, text)
  store.push(text)
  nil
end
store = StoredStrings.new
store.push(17)
text = +"SELECT"
original = text
store.push(text)
text += " WHERE"
forward_store(store, text)
p text, original, store.at(1), store.at(2)

# Read the old receiver before RHS rebinding, but its live bytes after RHS
# mutation. Asymmetric outputs distinguish rebinding from in-place append.
text = +"old"
store.push(text)
text += (text = +"replacement"; "!")
p text, store.at(3)
text = +"live"
other = text
store.push(text)
text += (other << "!"; "?")
p text, other, store.at(4)

text = "ice".freeze
store.push(text)
text += "+"
p text, store.at(5)
