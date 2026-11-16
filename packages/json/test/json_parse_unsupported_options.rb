# spinel: not-cruby -- Result-changing unsupported options raise by name; callback and custom-class support is separate.
require "json"
require "ostruct"

[{array_class: Object}, {decimal_class: Object}, {object_class: Object}, {create_additions: true}].each do |options|
  begin
    JSON.parse('[{"x":1.5}]', **options)
  rescue ArgumentError => error
    puts error.message
  end
end
[nil, false, ->(value) { value }].each do |callback|
  begin
    JSON.parse('[{"x":1}]', on_load: callback)
  rescue ArgumentError => error
    puts error.message
  end
end
options = {object_class: OpenStruct}
begin
  JSON.parse('{}', **options)
rescue ArgumentError => error
  puts error.message
end
p JSON.parse('[{"x":1}]')
p JSON.parse('{"x":1}', object_class: Hash)
p JSON.parse('{"x":1}', object_class: nil)
p JSON.parse('{"x":1}', object_class: false)
p JSON.parse('{"x":1}', object_class: OpenStruct).x
