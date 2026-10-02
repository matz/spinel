p "x" % []
p "%%" % []
empty = []
p "local" % empty
p ("chain" % []).upcase
p "%s:%d" % ["x", 2]
begin
  p "%s" % Array.new
rescue => error
  p [error.class, error.message]
end
begin
  p "%d" % []
rescue => error
  p [error.class, error.message]
end
def arguments
  print "A"
  []
end
p "effects" % arguments
def format_empty(s)
  s % []
end
p format_empty("ok")
begin
  format_empty(nil)
rescue NoMethodError => error
  p error.class
end
