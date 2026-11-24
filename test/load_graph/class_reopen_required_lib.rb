class Class
  def descendants
    found = subclasses
    found.each { |klass| found = found + klass.descendants }
    found
  end
end
