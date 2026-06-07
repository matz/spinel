RowA = Struct.new(:id, :name)
RowB = Struct.new(:id, :label)
require "json"

class Holder
  attributes :record, :records

  def initialize(record, records)
    @record = record
    @records = records
  end

  def record_json
    JSON.generate(@record)
  end

  def first_record_json
    JSON.generate(@records[0])
  end
end

def first_name(result)
  result.records[0].name
end

def first_label(result)
  result.records[0].label
end

holder_a = Holder.new(RowA.new(0, "empty-a"), [RowA.new(1, "alpha")])
holder_b = Holder.new(RowB.new(0, "empty-b"), [RowB.new(2, "beta")])

puts first_name(holder_a)
puts first_label(holder_b)
puts holder_a.records.length + holder_b.records.length
puts holder_a.record_json
puts holder_b.record_json
puts holder_a.first_record_json
puts holder_b.first_record_json
