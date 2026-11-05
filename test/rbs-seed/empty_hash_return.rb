# spinel: rbs-seed-run
module EmptyHashReturn
  def self.size(options) = options.length
  def self.generic_size(options) = options.length
  def self.empty = {}
  def self.constructed = Hash.new
  def self.nonempty = { "key" => "value" }

  def self.explicit
    return {}
  end

  def self.branch(flag)
    flag ? { "key" => "value" } : {}
  end

  def self.early(flag)
    return {} if flag
    { "key" => "value" }
  end

  def self.return_branch(flag)
    return flag ? {} : { "key" => "value" }
  end

  def self.nullable(flag)
    return nil unless flag
    {}
  end

  def self.ensured
    begin
      {}
    ensure
      puts "ensured"
    end
  end

  def self.rescued(flag)
    raise ArgumentError if flag
    {}
  rescue ArgumentError
    { "key" => "rescued" }
  end

  def self.counts(flag)
    flag ? {} : { "count" => 3 }
  end

  def self.mixed(flag)
    flag ? { "number" => 7, "text" => "seven" } : {}
  end

  def self.aliased
    options = { "number" => 9, "text" => "nine" }
    options
  end
end

p EmptyHashReturn.size(EmptyHashReturn.empty)
p EmptyHashReturn.size({})
p EmptyHashReturn.size(EmptyHashReturn.nonempty)
p EmptyHashReturn.generic_size(EmptyHashReturn.empty)
p EmptyHashReturn.constructed.length
p EmptyHashReturn.explicit.length
p EmptyHashReturn.branch(true).length
p EmptyHashReturn.branch(false).length
p EmptyHashReturn.early(true).length
p EmptyHashReturn.early(false).length
p EmptyHashReturn.return_branch(true).length
p EmptyHashReturn.return_branch(false).length
p EmptyHashReturn.nullable(false)
p EmptyHashReturn.nullable(true).length
p EmptyHashReturn.ensured.length
p EmptyHashReturn.rescued(false).length
p EmptyHashReturn.rescued(true)["key"]
p EmptyHashReturn.counts(true).length
p EmptyHashReturn.counts(false)["count"]
p EmptyHashReturn.mixed(true)["number"], EmptyHashReturn.mixed(true)["text"]
p EmptyHashReturn.mixed(false).length
p EmptyHashReturn.aliased["number"], EmptyHashReturn.aliased["text"]
