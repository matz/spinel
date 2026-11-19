$events = []

def strip(*) = :object

class String
  undef strip

  def method_missing(name, *args)
    $events << :handler
    [name, args]
  end
end

def receiver
  $events << :receiver
  [1, "abc"][1]
end

def argument(name)
  $events << name
  name
end

p receiver.strip(argument(:first), argument(:second))
p $events
