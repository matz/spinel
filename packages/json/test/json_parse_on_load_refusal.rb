# spinel: not-cruby -- on_load is refused explicitly until native callback argument binding is implemented.
require "json"

module NumberPolicy
  def self.check(value)
    raise ArgumentError, "non_integer" if value.is_a?(Float)
    value
  end
end

def refused(text, options)
  JSON.parse(text, **options)
  puts "accepted"
rescue ArgumentError => error
  puts error.message
end

refused('1.0', {on_load: NumberPolicy.method(:check)})
refused('[1,{"n":2}]', {on_load: proc { |value| value }})
refused('[1,{"n":2}]', {on_load: ->(value) { value }})
refused('1', {on_load: :to_s})
refused('1', {on_load: {1 => 2}})
options = {on_load: ->(value) { value }, allow_duplicate_key: false}
refused('{"x":1,"x":2}', options)
copy = options.dup
refused('[null,true,false]', copy)
refused('[]', {[:on_load].first => false})
begin
  ::JSON.parse('[]', on_load: nil)
rescue ArgumentError => error
  puts error.message
end
begin
  JSON.send(:parse, '[]', on_load: nil)
rescue ArgumentError => error
  puts error.message
end
p JSON.parse('[1,{"n":2}]')
