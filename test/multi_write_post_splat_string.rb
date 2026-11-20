class StringHoldingSink
  def value=(value)
    GC.start
    trash = []
    30.times { |i| trash << "churn-" + i.to_s }
    p [value, trash.length]
  end
end

sink = StringHoldingSink.new
*, sink.value, text = 0, +"b"
p text
text << "!"
p text

(*rest, last), final = [1, 2, 3], +"fresh"
p rest, last
final << "!"
p final
