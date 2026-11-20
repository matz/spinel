attempt = 0
events = []
word = +"kept"
values = [word].map do |item|
  begin
    begin
      attempt += 1
      events << attempt
      redo if attempt == 1
      item
    ensure
      events << :inner
      GC.start
    end
  ensure
    events << :outer
  end
end
p events, values.first.equal?(word)
