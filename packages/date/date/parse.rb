# frozen_string_literal: true

require_relative "zonetab"

class Date
  # Character class flags used by _parse
  HAVE_ALPHA = 1
  HAVE_DIGIT = 2
  HAVE_DASH  = 4
  HAVE_DOT   = 8
  HAVE_SLASH = 16
  HAVE_COLON = 32
  private_constant :HAVE_ALPHA, :HAVE_DIGIT, :HAVE_DASH, :HAVE_DOT, :HAVE_SLASH, :HAVE_COLON

  class << self

    # ------------------------------------------------------------------
    # _rfc3339
    # ------------------------------------------------------------------

    # call-seq:
    #   Date._rfc3339(string, limit: 128) -> hash
    #
    # Returns a hash of values parsed from +string+, which should be a valid
    # {RFC 3339 format}[rdoc-ref:language/strftime_formatting.rdoc@RFC+3339+Format]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.rfc3339     # => "2001-02-03T00:00:00+00:00"
    #   Date._rfc3339(s)
    #   # => {:year=>2001, :mon=>2, :mday=>3, :hour=>0, :min=>0, :sec=>0, :zone=>"+00:00", :offset=>0}
    #
    # See argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date.rfc3339 (returns a \Date object).
    def _rfc3339(string, limit: 128)
      return {} if string.nil?
      string = string_value(string)
      return {} if string.empty?
      raise ArgumentError, "string length (#{string.length}) exceeds the limit #{limit}" if limit && string.length > limit

      # Fast path: YYYY-MM-DDTHH:MM:SS+HH:MM (25 bytes) or YYYY-MM-DDTHH:MM:SSZ (20 bytes)
      len = string.length
      if len == 25 || len == 20
        sc = StringScanner.new(string.dup)
        if sc.scan(/(\d{4})-(\d{2})-(\d{2})[Tt](\d{2}):(\d{2}):(\d{2})/)
          h = {
            year: sc[1].to_i, mon: sc[2].to_i, mday: sc[3].to_i,
            hour: sc[4].to_i, min: sc[5].to_i, sec: sc[6].to_i
          }
          if sc.scan(/[Zz]\z/)
            h[:zone] = sc.matched
            h[:offset] = 0
            return h
          elsif sc.scan(/([+-])(\d{2}):(\d{2})\z/)
            zone = sc.matched
            h[:zone] = zone
            h[:offset] = (sc[1] == '-' ? -1 : 1) * (sc[2].to_i * 3600 + sc[3].to_i * 60)
            return h
          end
        end
      end

      h = {}
      if (m = RFC3339_RE.match(string))
        h[:year]  = m[1].to_i
        h[:mon]   = m[2].to_i
        h[:mday]  = m[3].to_i
        h[:hour]  = m[4].to_i
        h[:min]   = m[5].to_i
        h[:sec]   = m[6].to_i
        h[:sec_fraction] = Rational(m[7].to_i, 10 ** m[7].length) if m[7]
        zone = m[8]
        h[:zone] = zone
        if zone[0] == 'Z' || zone[0] == 'z'
          h[:offset] = 0
        else
          h[:offset] = (zone[0] == '-' ? -1 : 1) * (zone[1, 2].to_i * 3600 + zone[4, 2].to_i * 60)
        end
      end
      h
    end

    # ------------------------------------------------------------------
    # _httpdate
    # ------------------------------------------------------------------

    # call-seq:
    #   Date._httpdate(string, limit: 128) -> hash
    #
    # Returns a hash of values parsed from +string+, which should be a valid
    # {HTTP date format}[rdoc-ref:language/strftime_formatting.rdoc@HTTP+Format]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.httpdate # => "Sat, 03 Feb 2001 00:00:00 GMT"
    #   Date._httpdate(s)
    #   # => {:wday=>6, :mday=>3, :mon=>2, :year=>2001, :hour=>0, :min=>0, :sec=>0, :zone=>"GMT", :offset=>0}
    #
    # Related: Date.httpdate (returns a \Date object).
    def _httpdate(string, limit: 128)
      return {} if string.nil?
      string = string_value(string)
      return {} if string.empty?
      raise ArgumentError, "string length (#{string.length}) exceeds the limit #{limit}" if limit && string.length > limit

      # Fast path for Type 1: "Dow, DD Mon YYYY HH:MM:SS GMT" (29 bytes)
      len = string.length
      if len == 29
        sc = StringScanner.new(string.dup)
        if sc.scan(/([A-Za-z]{3}), (\d{2}) ([A-Za-z]{3}) (\d{4}) (\d{2}):(\d{2}):(\d{2}) (GMT)\z/i)
          wkey = compute_3key(sc[1])
          wday_info = ABBR_DAY_3KEY[wkey]
          if wday_info
            mkey = compute_3key(sc[3])
            mon_info = ABBR_MONTH_3KEY[mkey]
            if mon_info
              return {
                wday: wday_info[0], mday: sc[2].to_i, mon: mon_info[0],
                year: sc[4].to_i, hour: sc[5].to_i, min: sc[6].to_i, sec: sc[7].to_i,
                zone: 'GMT', offset: 0
              }
            end
          end
        end
      end

      h = {}
      if (m = HTTPDATE_TYPE1_RE.match(string))
        h[:wday]   = HTTPDATE_WDAY[m[1].downcase]
        h[:mday]   = m[2].to_i
        h[:mon]    = ABBR_MONTH_NUM[m[3].downcase]
        h[:year]   = m[4].to_i
        h[:hour]   = m[5].to_i
        h[:min]    = m[6].to_i
        h[:sec]    = m[7].to_i
        h[:zone]   = m[8]
        h[:offset] = 0
      elsif (m = HTTPDATE_TYPE2_RE.match(string))
        h[:wday]   = HTTPDATE_FULL_WDAY[m[1].downcase]
        h[:mday]   = m[2].to_i
        h[:mon]    = ABBR_MONTH_NUM[m[3].downcase]
        y = m[4].to_i
        h[:year]   = y >= 69 ? y + 1900 : y + 2000
        h[:hour]   = m[5].to_i
        h[:min]    = m[6].to_i
        h[:sec]    = m[7].to_i
        h[:zone]   = m[8]
        h[:offset] = 0
      elsif (m = HTTPDATE_TYPE3_RE.match(string))
        h[:wday]   = HTTPDATE_WDAY[m[1].downcase]
        h[:mon]    = ABBR_MONTH_NUM[m[2].downcase]
        h[:mday]   = m[3].to_i
        h[:hour]   = m[4].to_i
        h[:min]    = m[5].to_i
        h[:sec]    = m[6].to_i
        h[:year]   = m[7].to_i
      end
      h
    end

    # ------------------------------------------------------------------
    # _rfc2822
    # ------------------------------------------------------------------

    # call-seq:
    #   Date._rfc2822(string, limit: 128) -> hash
    #
    # Returns a hash of values parsed from +string+, which should be a valid
    # {RFC 2822 date format}[rdoc-ref:language/strftime_formatting.rdoc@RFC+2822+Format]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.rfc2822 # => "Sat, 3 Feb 2001 00:00:00 +0000"
    #   Date._rfc2822(s)
    #   # => {:wday=>6, :mday=>3, :mon=>2, :year=>2001, :hour=>0, :min=>0, :sec=>0, :zone=>"+0000", :offset=>0}
    #
    # See argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date.rfc2822 (returns a \Date object).
    def _rfc2822(string, limit: 128)
      return {} if string.nil?
      string = string_value(string)
      return {} if string.empty?
      raise ArgumentError, "string length (#{string.length}) exceeds the limit #{limit}" if limit && string.length > limit

      # Fast path: "Dow, DD Mon YYYY HH:MM:SS +ZZZZ" (31 bytes, 2-digit day)
      len = string.length
      if len == 31
        sc = StringScanner.new(string.dup)
        if sc.scan(/([A-Za-z]{3}), (\d{2}) ([A-Za-z]{3}) (\d{4}) (\d{2}):(\d{2}):(\d{2}) ([+-]\d{4})\z/)
          wkey = compute_3key(sc[1])
          wday_info = ABBR_DAY_3KEY[wkey]
          if wday_info
            mkey = compute_3key(sc[3])
            mon_info = ABBR_MONTH_3KEY[mkey]
            if mon_info
              zone = sc[8]
              sign = zone[0] == '-' ? -1 : 1
              offset_val = sign * (zone[1, 2].to_i * 3600 + zone[3, 2].to_i * 60)
              zone = '+0000' if zone == '+0000'
              return {
                wday: wday_info[0], mday: sc[2].to_i, mon: mon_info[0],
                year: sc[4].to_i, hour: sc[5].to_i, min: sc[6].to_i, sec: sc[7].to_i,
                zone: zone, offset: offset_val
              }
            end
          end
        end
      end

      # Preprocess: remove obs-FWS (\r\n and \0) - skip if not needed
      s = (string.include?("\r") || string.include?("\n") || string.include?("\0")) ?
          string.gsub(/[\r\n\0]+/, ' ') : string
      h = {}
      if (m = RFC2822_RE.match(s))
        h[:wday]   = HTTPDATE_WDAY[m[1].downcase] if m[1]
        h[:mday]   = m[2].to_i
        h[:mon]    = ABBR_MONTH_NUM[m[3].downcase]
        y_s = m[4]
        y = y_s.to_i
        ylen = y_s[0] == '-' ? y_s.length - 1 : y_s.length
        if ylen < 4
          if ylen == 3
            h[:year] = y + 1900
          else
            h[:year] = y >= 50 ? y + 1900 : y + 2000
          end
        else
          h[:year] = y
        end
        h[:hour]   = m[5].to_i
        h[:min]    = m[6].to_i
        h[:sec]    = m[7].to_i if m[7]
        h[:zone]   = m[8]
        h[:offset] = fast_zone_offset(m[8])
      end
      h
    end
    alias _rfc822 _rfc2822

    # ------------------------------------------------------------------
    # _xmlschema
    # ------------------------------------------------------------------

    # call-seq:
    #   Date._xmlschema(string, limit: 128) -> hash
    #
    # Returns a hash of values parsed from +string+, which should be a valid
    # XML date format:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.xmlschema    # => "2001-02-03"
    #   Date._xmlschema(s) # => {:year=>2001, :mon=>2, :mday=>3}
    #
    # See argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date.xmlschema (returns a \Date object).
    def _xmlschema(string, limit: 128)
      return {} if string.nil?
      string = string_value(string)
      return {} if string.empty?
      raise ArgumentError, "string length (#{string.length}) exceeds the limit #{limit}" if limit && string.length > limit

      # Fast path: YYYY-MM-DD (exactly 10 bytes, all ASCII)
      if string.length == 10
        sc = StringScanner.new(string.dup)
        if sc.scan(/(\d{4})-(\d{2})-(\d{2})\z/)
          return { year: sc[1].to_i, mon: sc[2].to_i, mday: sc[3].to_i }
        end
      end

      h = {}
      if (m = XMLSCHEMA_DATETIME_RE.match(string))
        h[:year]  = m[1].to_i
        h[:mon]   = m[2].to_i if m[2]
        h[:mday]  = m[3].to_i if m[3]
        h[:hour]  = m[4].to_i if m[4]
        h[:min]   = m[5].to_i if m[5]
        h[:sec]   = m[6].to_i if m[6]
        h[:sec_fraction] = parse_sec_fraction(m[7]) if m[7]
        parse_zone_and_offset(m[8], h) if m[8]
      elsif (m = XMLSCHEMA_TIME_RE.match(string))
        h[:hour]  = m[1].to_i
        h[:min]   = m[2].to_i
        h[:sec]   = m[3].to_i
        h[:sec_fraction] = parse_sec_fraction(m[4]) if m[4]
        parse_zone_and_offset(m[5], h) if m[5]
      elsif (m = XMLSCHEMA_TRUNC_RE.match(string))
        if m[3]
          h[:mday] = m[3].to_i
        else
          h[:mon]  = m[1].to_i if m[1]
          h[:mday] = m[2].to_i if m[2]
        end
        parse_zone_and_offset(m[4], h) if m[4]
      end
      h
    end

    # ------------------------------------------------------------------
    # _iso8601
    # ------------------------------------------------------------------

    # call-seq:
    #   Date._iso8601(string, limit: 128) -> hash
    #
    # Returns a hash of values parsed from +string+, which should contain
    # an {ISO 8601 formatted date}[rdoc-ref:language/strftime_formatting.rdoc@ISO+8601+Format+Specifications]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.iso8601    # => "2001-02-03"
    #   Date._iso8601(s) # => {:mday=>3, :year=>2001, :mon=>2}
    #
    # See argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date.iso8601 (returns a \Date object).
    def _iso8601(string, limit: 128)
      return {} if string.nil?
      string = string_value(string)
      return {} if string.empty?
      raise ArgumentError, "string length (#{string.length}) exceeds the limit #{limit}" if limit && string.length > limit

      # Fast path: YYYY-MM-DD (exactly 10 bytes, all ASCII)
      if string.length == 10
        sc = StringScanner.new(string.dup)
        if sc.scan(/(\d{4})-(\d{2})-(\d{2})\z/)
          return { mday: sc[3].to_i, year: sc[1].to_i, mon: sc[2].to_i }
        end
      end

      h = {}

      if (m = ISO8601_EXT_DATETIME_RE.match(string))
        iso8601_ext_datetime(m, h)
      elsif (m = ISO8601_BAS_DATETIME_RE.match(string))
        iso8601_bas_datetime(m, h)
      elsif (m = ISO8601_EXT_TIME_RE.match(string))
        h[:hour] = m[1].to_i
        h[:min]  = m[2].to_i
        h[:sec]  = m[3].to_i if m[3]
        h[:sec_fraction] = parse_sec_fraction(m[4]) if m[4]
        parse_zone_and_offset(m[5], h) if m[5]
      elsif (m = ISO8601_BAS_TIME_RE.match(string))
        h[:hour] = m[1].to_i
        h[:min]  = m[2].to_i
        h[:sec]  = m[3].to_i if m[3]
        h[:sec_fraction] = parse_sec_fraction(m[4]) if m[4]
        parse_zone_and_offset(m[5], h) if m[5]
      end
      h
    end

    # ------------------------------------------------------------------
    # _jisx0301
    # ------------------------------------------------------------------

    # call-seq:
    #   Date._jisx0301(string, limit: 128) -> hash
    #
    # Returns a hash of values parsed from +string+, which should be a valid
    # {JIS X 0301 date format}[rdoc-ref:language/strftime_formatting.rdoc@JIS+X+0301+Format]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.jisx0301    # => "H13.02.03"
    #   Date._jisx0301(s) # => {:year=>2001, :mon=>2, :mday=>3}
    #
    # See argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date.jisx0301 (returns a \Date object).
    def _jisx0301(string, limit: 128)
      return {} if string.nil?
      string = string_value(string)
      return {} if string.empty?
      raise ArgumentError, "string length (#{string.length}) exceeds the limit #{limit}" if limit && string.length > limit

      # Fast path: X##.##.## (9 bytes: era + YY.MM.DD)
      if string.length == 9
        sc = StringScanner.new(string.dup)
        if sc.scan(/([A-Za-z])(\d{2})\.(\d{2})\.(\d{2})\z/)
          era_offset = JISX0301_ERA[sc[1].downcase]
          if era_offset
            return {
              year: sc[2].to_i + era_offset,
              mon:  sc[3].to_i,
              mday: sc[4].to_i
            }
          end
        end
      end

      h = {}
      if (m = JISX0301_RE.match(string))
        era_char = m[1] ? m[1].downcase : 'h'
        era_offset = JISX0301_ERA[era_char]
        h[:year] = m[2].to_i + era_offset
        h[:mon]  = m[3].to_i
        h[:mday] = m[4].to_i
        h[:hour] = m[5].to_i if m[5]
        h[:min]  = m[6].to_i if m[6]
        h[:sec]  = m[7].to_i if m[7]
        h[:sec_fraction] = parse_sec_fraction(m[8]) if m[8] && !m[8].empty?
        parse_zone_and_offset(m[9], h) if m[9]
      else
        h = _iso8601(string, limit: limit)
      end
      h
    end

    # ------------------------------------------------------------------
    # _parse
    # ------------------------------------------------------------------

    # call-seq:
    #   Date._parse(string, comp = true, limit: 128) -> hash
    #
    # <b>Note</b>:
    # This method recognizes many forms in +string+,
    # but it is not a validator.
    # For formats, see
    # {"Specialized Format Strings" in Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc@Specialized+Format+Strings]
    #
    # If +string+ does not specify a valid date,
    # the result is unpredictable;
    # consider using Date._strptime instead.
    #
    # Returns a hash of values parsed from +string+:
    #
    #   Date._parse('2001-02-03') # => {:year=>2001, :mon=>2, :mday=>3}
    #
    # If +comp+ is +true+ and the given year is in the range <tt>(0..99)</tt>,
    # the current century is supplied;
    # otherwise, the year is taken as given:
    #
    #   Date._parse('01-02-03', true)  # => {:year=>2001, :mon=>2, :mday=>3}
    #   Date._parse('01-02-03', false) # => {:year=>1, :mon=>2, :mday=>3}
    #
    # See argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date.parse(returns a \Date object).
    def _parse(string, comp = true, limit: 128)
      string = string_value(string)
      return {} if string.empty?
      raise ArgumentError, "string length (#{string.length}) exceeds limit (#{limit})" if limit && string.length > limit

      # === Fast paths for common date formats ===
      len = string.length

      # Fast ISO: YYYY-MM-DD (exactly 10 ASCII bytes)
      if len == 10
        sc = StringScanner.new(string.dup)
        if sc.scan(/(\d{4})-(\d{2})-(\d{2})\z/)
          return { year: sc[1].to_i, mon: sc[2].to_i, mday: sc[3].to_i }
        end
      end

      # Fast compact: YYYYMMDD (exactly 8 ASCII digit bytes)
      if len == 8
        sc = StringScanner.new(string.dup)
        if sc.scan(/(\d{4})(\d{2})(\d{2})\z/)
          return { year: sc[1].to_i, mon: sc[2].to_i, mday: sc[3].to_i }
        end
      end

      # Fast US: "Month DD, YYYY"
      if (m = FAST_PARSE_US_RE.match(string))
        mon = ABBR_MONTH_NUM[m[1].downcase]
        return {year: m[3].to_i, mon: mon, mday: m[2].to_i} if mon
      end

      # Fast EU: "DD Month YYYY"
      if (m = FAST_PARSE_EU_RE.match(string))
        mon = ABBR_MONTH_NUM[m[2].downcase]
        return {year: m[3].to_i, mon: mon, mday: m[1].to_i} if mon
      end

      # Fast RFC2822-like: "Dow, DD Mon YYYY HH:MM:SS +ZZZZ"
      if (m = FAST_PARSE_RFC2822_RE.match(string))
        wday = ABBR_DAY_NUM[m[1].downcase]
        mon  = ABBR_MONTH_NUM[m[3].downcase]
        if wday && mon
          zone = m[8]
          return {
            wday: wday, mday: m[2].to_i, mon: mon, year: m[4].to_i,
            hour: m[5].to_i, min: m[6].to_i, sec: m[7].to_i,
            zone: zone, offset: fast_zone_offset(zone)
          }
        end
      end

      # Preprocessing: replace non-date chars with space
      str = string.dup
      str.gsub!(/[^-+',.\/:\@\[\][:alnum:]]+/, ' ')

      # check_class (byte-level for speed)
      cc = 0
      str.each_byte do |b|
        if b >= 65 && b <= 90 || b >= 97 && b <= 122 || b > 127
          cc |= HAVE_ALPHA
        elsif b >= 48 && b <= 57
          cc |= HAVE_DIGIT
        elsif b == 45
          cc |= HAVE_DASH
        elsif b == 46
          cc |= HAVE_DOT
        elsif b == 47
          cc |= HAVE_SLASH
        elsif b == 58
          cc |= HAVE_COLON
        end
      end

      h = {}

      # parse_day (always runs)
      if (cc & HAVE_ALPHA) != 0
        parse_day(str, h)
      end

      # parse_time (needs colon or alpha for h/am/pm patterns)
      if (cc & HAVE_DIGIT) != 0 && (cc & (HAVE_COLON | HAVE_ALPHA)) != 0
        parse_time(str, h)
      end

      # Date parsers: first match wins (goto ok)
      matched = false

      if !matched && (cc & (HAVE_ALPHA | HAVE_DIGIT)) == (HAVE_ALPHA | HAVE_DIGIT)
        matched = parse_eu(str, h)
      end

      if !matched && (cc & (HAVE_ALPHA | HAVE_DIGIT)) == (HAVE_ALPHA | HAVE_DIGIT)
        matched = parse_us(str, h)
      end

      if !matched && (cc & (HAVE_DIGIT | HAVE_DASH)) == (HAVE_DIGIT | HAVE_DASH)
        matched = parse_iso(str, h)
      end

      if !matched && (cc & (HAVE_DIGIT | HAVE_DOT)) == (HAVE_DIGIT | HAVE_DOT)
        matched = parse_jis(str, h)
      end

      if !matched && (cc & (HAVE_ALPHA | HAVE_DIGIT | HAVE_DASH)) == (HAVE_ALPHA | HAVE_DIGIT | HAVE_DASH)
        matched = parse_vms(str, h)
      end

      if !matched && (cc & (HAVE_DIGIT | HAVE_SLASH)) == (HAVE_DIGIT | HAVE_SLASH)
        matched = parse_sla(str, h)
      end

      if !matched && (cc & (HAVE_DIGIT | HAVE_DOT)) == (HAVE_DIGIT | HAVE_DOT)
        matched = parse_dot(str, h)
      end

      if !matched && (cc & HAVE_DIGIT) != 0
        matched = parse_iso2(str, h)
      end

      if !matched && (cc & HAVE_DIGIT) != 0
        matched = parse_year(str, h)
      end

      if !matched && (cc & HAVE_ALPHA) != 0
        matched = parse_mon(str, h)
      end

      if !matched && (cc & HAVE_DIGIT) != 0
        matched = parse_mday(str, h)
      end

      if !matched && (cc & HAVE_DIGIT) != 0
        parse_ddd(str, h)
      end

      # Post-processing (always runs after ok label)
      # parse_bc
      if (cc & HAVE_ALPHA) != 0
        parse_bc_post(str, h)
      end

      # parse_frag
      if (cc & HAVE_DIGIT) != 0
        parse_frag(str, h)
      end

      # BC handling
      if h.delete(:_bc)
        h[:cwyear] = -h[:cwyear] + 1 if h[:cwyear]
        h[:year]   = -h[:year] + 1    if h[:year]
      end

      # comp (century completion)
      if comp && h.delete(:_comp) != false
        [:cwyear, :year].each do |key|
          y = h[key]
          if y && y >= 0 && y <= 99
            h[key] = y >= 69 ? y + 1900 : y + 2000
          end
        end
      end

      # zone -> offset
      # (String() answers the zone itself; it tells the compiler what the
      # hash's value is)
      if h[:zone] && !h.key?(:offset)
        h[:offset] = fast_zone_offset(String(h[:zone]))
      end

      h
    end

    # ------------------------------------------------------------------
    # parse constructor
    # ------------------------------------------------------------------

    # call-seq:
    #   Date.parse(string = '-4712-01-01', comp = true, start = Date::ITALY, limit: 128) -> date
    #
    # <b>Note</b>:
    # This method recognizes many forms in +string+,
    # but it is not a validator.
    # For formats, see
    # {"Specialized Format Strings" in Formats for Dates and Times}[rdoc-ref:language/strftime_formatting.rdoc@Specialized+Format+Strings]
    # If +string+ does not specify a valid date,
    # the result is unpredictable;
    # consider using Date._strptime instead.
    #
    # Returns a new \Date object with values parsed from +string+:
    #
    #   Date.parse('2001-02-03')   # => #<Date: 2001-02-03>
    #   Date.parse('20010203')     # => #<Date: 2001-02-03>
    #   Date.parse('3rd Feb 2001') # => #<Date: 2001-02-03>
    #
    # If +comp+ is +true+ and the given year is in the range <tt>(0..99)</tt>,
    # the current century is supplied;
    # otherwise, the year is taken as given:
    #
    #   Date.parse('01-02-03', true)  # => #<Date: 2001-02-03>
    #   Date.parse('01-02-03', false) # => #<Date: 0001-02-03>
    #
    # See:
    #
    # - Argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    # - Argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date._parse (returns a hash).
    def parse(string = '-4712-01-01', comp = true, start = DEFAULT_SG, limit: 128)
      hash = _parse(string, comp, limit: limit)
      fast_new_date(hash, start)
    end

    # ------------------------------------------------------------------
    # Specialized constructors
    # ------------------------------------------------------------------

    # call-seq:
    #   Date.iso8601(string = '-4712-01-01', start = Date::ITALY, limit: 128) -> date
    #
    # Returns a new \Date object with values parsed from +string+,
    # which should contain
    # an {ISO 8601 formatted date}[rdoc-ref:language/strftime_formatting.rdoc@ISO+8601+Format+Specifications]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.iso8601   # => "2001-02-03"
    #   Date.iso8601(s) # => #<Date: 2001-02-03>
    #
    # See:
    #
    # - Argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    # - Argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date._iso8601 (returns a hash).
    def iso8601(string = JULIAN_EPOCH_DATE, start = DEFAULT_SG, limit: 128)
      hash = _iso8601(string, limit: limit)
      fast_new_date(hash, start)
    end

    # call-seq:
    #   Date.rfc3339(string = '-4712-01-01T00:00:00+00:00', start = Date::ITALY, limit: 128) -> date
    #
    # Returns a new \Date object with values parsed from +string+,
    # which should be a valid
    # {RFC 3339 format}[rdoc-ref:language/strftime_formatting.rdoc@RFC+3339+Format]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.rfc3339   # => "2001-02-03T00:00:00+00:00"
    #   Date.rfc3339(s) # => #<Date: 2001-02-03>
    #
    # See:
    #
    # - Argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    # - Argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date._rfc3339 (returns a hash).
    def rfc3339(string = JULIAN_EPOCH_DATETIME, start = DEFAULT_SG, limit: 128)
      hash = _rfc3339(string, limit: limit)
      fast_new_date(hash, start)
    end

    # call-seq:
    #   Date.xmlschema(string = '-4712-01-01', start = Date::ITALY, limit: 128)  ->  date
    #
    # Returns a new \Date object with values parsed from +string+,
    # which should be a valid XML date format:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.xmlschema   # => "2001-02-03"
    #   Date.xmlschema(s) # => #<Date: 2001-02-03>
    #
    # See:
    #
    # - Argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    # - Argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date._xmlschema (returns a hash).
    def xmlschema(string = JULIAN_EPOCH_DATE, start = DEFAULT_SG, limit: 128)
      hash = _xmlschema(string, limit: limit)
      fast_new_date(hash, start)
    end

    # call-seq:
    #   Date.rfc2822(string = 'Mon, 1 Jan -4712 00:00:00 +0000', start = Date::ITALY, limit: 128) -> date
    #
    # Returns a new \Date object with values parsed from +string+,
    # which should be a valid
    # {RFC 2822 date format}[rdoc-ref:language/strftime_formatting.rdoc@RFC+2822+Format]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.rfc2822   # => "Sat, 3 Feb 2001 00:00:00 +0000"
    #   Date.rfc2822(s) # => #<Date: 2001-02-03>
    #
    # See:
    #
    # - Argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    # - Argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date._rfc2822 (returns a hash).
    def rfc2822(string = JULIAN_EPOCH_DATETIME_RFC2822, start = DEFAULT_SG, limit: 128)
      hash = _rfc2822(string, limit: limit)
      fast_new_date(hash, start)
    end
    alias rfc822 rfc2822

    # call-seq:
    #   Date.httpdate(string = 'Mon, 01 Jan -4712 00:00:00 GMT', start = Date::ITALY, limit: 128) -> date
    #
    # Returns a new \Date object with values parsed from +string+,
    # which should be a valid
    # {HTTP date format}[rdoc-ref:language/strftime_formatting.rdoc@HTTP+Format]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.httpdate   # => "Sat, 03 Feb 2001 00:00:00 GMT"
    #   Date.httpdate(s) # => #<Date: 2001-02-03>
    #
    # See:
    #
    # - Argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    # - Argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date._httpdate (returns a hash).
    def httpdate(string = JULIAN_EPOCH_DATETIME_HTTPDATE, start = DEFAULT_SG, limit: 128)
      hash = _httpdate(string, limit: limit)
      fast_new_date(hash, start)
    end

    # call-seq:
    #   Date.jisx0301(string = '-4712-01-01', start = Date::ITALY, limit: 128) -> date
    #
    # Returns a new \Date object with values parsed from +string+,
    # which should be a valid {JIS X 0301 format}[rdoc-ref:language/strftime_formatting.rdoc@JIS+X+0301+Format]:
    #
    #   d = Date.new(2001, 2, 3)
    #   s = d.jisx0301   # => "H13.02.03"
    #   Date.jisx0301(s) # => #<Date: 2001-02-03>
    #
    # For no-era year, legacy format, Heisei is assumed.
    #
    #   Date.jisx0301('13.02.03') # => #<Date: 2001-02-03>
    #
    # See:
    #
    # - Argument {start}[rdoc-ref:language/calendars.rdoc@Argument+start].
    # - Argument {limit}[rdoc-ref:Date@Argument+limit].
    #
    # Related: Date._jisx0301 (returns a hash).
    def jisx0301(string = JULIAN_EPOCH_DATE, start = DEFAULT_SG, limit: 128)
      hash = _jisx0301(string, limit: limit)
      fast_new_date(hash, start)
    end

    private

    # ------------------------------------------------------------------
    # Shared infrastructure
    # ------------------------------------------------------------------

    def parse_check_limit(str, limit)
      raise ArgumentError, "string length (#{str.length}) exceeds limit (#{limit})" if limit && str.length > limit
    end

    # CRuby's StringValue: a String, or the String to_str gives, or a TypeError.
    def string_value(obj)
      case obj
      when String then return obj
      when nil, true, false then raise TypeError, "no implicit conversion of #{obj.inspect} into String"
      end
      if obj.respond_to?(:to_str)
        str = obj.to_str
        return str if String === str
        raise TypeError, "can't convert #{obj.class} to String (#{obj.class}#to_str gives #{str.class})"
      end
      raise TypeError, "no implicit conversion of #{obj.class} into String"
    end

    def parse_zone_and_offset(zone_str, hash)
      return unless zone_str
      hash[:zone] = zone_str
      hash[:offset] = fast_zone_offset(zone_str)
    end

    def parse_sec_fraction(frac_str)
      Rational(frac_str.to_i, 10 ** frac_str.length)
    end

    # Fast zone offset calculation for common patterns.
    # Handles: Z/z, +HH:MM/-HH:MM, +HHMM/-HHMM, short named zones.
    # Falls back to sp_zone_to_diff for complex cases.
    def fast_zone_offset(zone_str)
      len = zone_str.length

      # Z/z
      return 0 if len == 1 && (zone_str[0] == 'Z' || zone_str[0] == 'z')

      if zone_str[0] == '+' || zone_str[0] == '-'
        sc = StringScanner.new(zone_str.dup)
        if sc.scan(/([+-])(\d{2}):?(\d{2})\z/)
          sign = sc[1] == '-' ? -1 : 1
          return sign * (sc[2].to_i * 3600 + sc[3].to_i * 60)
        end
      end

      # Short named zones: gmt, utc, est, etc.
      if len <= 3
        off = ZONE_TABLE[zone_str.downcase]
        return off if off
      end

      # Fall back to full parser
      sp_zone_to_diff(zone_str)
    end

    # ------------------------------------------------------------------
    # _iso8601 helpers
    # ------------------------------------------------------------------

    def comp_year69(y)
      y >= 69 ? y + 1900 : y + 2000
    end

    def iso8601_ext_datetime(m, h)
      if m[1]
        # year-mon-mday or truncated
        unless m[1] == '-'
          y = m[1].to_i
          h[:year] = (m[1].length <= 2 && !m[1].start_with?('+') && !m[1].start_with?('-')) ? comp_year69(y) : y
        end
        h[:mon]  = m[2].to_i if m[2]
        h[:mday] = m[3].to_i if m[3]
      elsif m[4] || m[5]
        # year-yday
        if m[4]
          y = m[4].to_i
          h[:year] = (m[4].length <= 2 && !m[4].start_with?('+') && !m[4].start_with?('-')) ? comp_year69(y) : y
        end
        h[:yday] = m[5].to_i
      elsif m[6] || m[7]
        # cwyear-wNN-D
        if m[6]
          y = m[6].to_i
          h[:cwyear] = m[6].length <= 2 ? comp_year69(y) : y
        end
        h[:cweek] = m[7].to_i
        h[:cwday] = m[8].to_i if m[8]
      elsif m[9]
        # -w-D
        h[:cwday] = m[9].to_i
      end
      # time part
      if m[10]
        h[:hour] = m[10].to_i
        h[:min]  = m[11].to_i if m[11]
        h[:sec]  = m[12].to_i if m[12]
        h[:sec_fraction] = parse_sec_fraction(m[13]) if m[13]
        parse_zone_and_offset(m[14], h) if m[14]
      end
    end

    def iso8601_bas_datetime(m, h)
      if m[1]
        # yyyymmdd / --mmdd / ----dd
        unless m[1] == '--'
          y_s = m[1]
          y = y_s.to_i
          ylen = y_s.sub(/\A[-+]/, '').length
          h[:year] = (ylen <= 2 && !y_s.start_with?('+') && !y_s.start_with?('-')) ? comp_year69(y) : y
        end
        h[:mon]  = m[2].to_i unless m[2] == '-'
        h[:mday] = m[3].to_i
      elsif m[4]
        # yyyyddd
        y_s = m[4]
        y = y_s.to_i
        ylen = y_s.sub(/\A[-+]/, '').length
        h[:year] = (ylen <= 2 && !y_s.start_with?('+') && !y_s.start_with?('-')) ? comp_year69(y) : y
        h[:yday] = m[5].to_i
      elsif m[6]
        # -ddd
        h[:yday] = m[6].to_i
      elsif m[7]
        # yyyywwwd
        y = m[7].to_i
        h[:cwyear] = m[7].length <= 2 ? comp_year69(y) : y
        h[:cweek]  = m[8].to_i
        h[:cwday]  = m[9].to_i
      elsif m[10]
        # -wNN-D
        h[:cweek] = m[10].to_i
        h[:cwday] = m[11].to_i
      elsif m[12]
        # -w-D
        h[:cwday] = m[12].to_i
      end
      # time part
      if m[13]
        h[:hour] = m[13].to_i
        h[:min]  = m[14].to_i if m[14]
        h[:sec]  = m[15].to_i if m[15]
        h[:sec_fraction] = parse_sec_fraction(m[16]) if m[16]
        parse_zone_and_offset(m[17], h) if m[17]
      end
    end

    # ------------------------------------------------------------------
    # _parse sub-parsers (private)
    # ------------------------------------------------------------------

    def parse_day(str, h)
      if (m = PARSE_DAYS_RE.match(str))
        h[:wday] = ABBR_DAY_NUM[m[1].downcase]
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
      end
    end

    def parse_time(str, h)
      if (m = PARSE_TIME_RE.match(str))
        time_part = m[1]
        zone_part = m[2]
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))

        if (tm = PARSE_TIME_CB_RE.match(time_part))
          hour = tm[1].to_i
          if tm[5]
            ampm = tm[5].downcase
            if ampm == 'p'
              hour = (hour % 12) + 12
            else
              hour = hour % 12
            end
          end
          h[:hour] = hour
          h[:min]  = tm[2].to_i if tm[2]
          h[:sec]  = tm[3].to_i if tm[3]
          h[:sec_fraction] = parse_sec_fraction(tm[4]) if tm[4]
        end

        if zone_part
          h[:zone] = zone_part
        end
      end
    end

    def parse_eu(str, h)
      if (m = PARSE_EU_RE.match(str))
        mon = ABBR_MONTH_NUM[m[2].downcase]
        return false unless mon
        bc = m[3] && m[3] =~ /\Ab/i ? true : false
        s3e(h, m[4], mon, m[1], bc)
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_us(str, h)
      if (m = PARSE_US_RE.match(str))
        mon = ABBR_MONTH_NUM[m[1].downcase]
        return false unless mon
        bc = m[3] && m[3] =~ /\Ab/i ? true : false
        s3e(h, m[4], mon, m[2], bc)
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_iso(str, h)
      if (m = PARSE_ISO_RE.match(str))
        y_s = m[1]
        m_s = m[2]
        d_s = m[3]
        # Fast path: y is unambiguous year (3+ digits or signed), d is short
        if y_s =~ /\A[-+]?\d{3,}\z/ && d_s =~ /\A\d{1,2}\z/
          h[:year] = y_s.to_i
          h[:_comp] = false
          h[:mon] = m_s.to_i
          h[:mday] = d_s.to_i
        else
          s3e(h, y_s, m_s, d_s, false)
        end
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_jis(str, h)
      if (m = PARSE_JIS_RE.match(str))
        era_char = m[1].downcase
        era_offset = JISX0301_ERA[era_char]
        return false unless era_offset
        h[:year] = m[2].to_i + era_offset
        h[:mon]  = m[3].to_i
        h[:mday] = m[4].to_i
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_vms(str, h)
      if (m = PARSE_VMS11_RE.match(str))
        mon = ABBR_MONTH_NUM[m[2].downcase]
        return false unless mon
        s3e(h, m[3], mon, m[1], false)
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      elsif (m = PARSE_VMS12_RE.match(str))
        mon = ABBR_MONTH_NUM[m[1].downcase]
        return false unless mon
        s3e(h, m[3], mon, m[2], false)
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_sla(str, h)
      if (m = PARSE_SLA_RE.match(str))
        s3e(h, m[1], m[2], m[3], false)
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_dot(str, h)
      if (m = PARSE_DOT_RE.match(str))
        s3e(h, m[1], m[2], m[3], false)
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_iso2(str, h)
      # iso21: week date
      if (m = PARSE_ISO21_RE.match(str))
        if m[1]
          y = m[1].to_i
          h[:cwyear] = y
        end
        h[:cweek] = m[2].to_i
        h[:cwday] = m[3].to_i if m[3]
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        return true
      end

      # iso22: -w-D
      if (m = PARSE_ISO22_RE.match(str))
        h[:cwday] = m[1].to_i
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        return true
      end

      # iso23: --MM-DD
      if (m = PARSE_ISO23_RE.match(str))
        h[:mon]  = m[1].to_i if m[1]
        h[:mday] = m[2].to_i
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        return true
      end

      # iso24: --MMDD
      if (m = PARSE_ISO24_RE.match(str))
        h[:mon]  = m[1].to_i
        h[:mday] = m[2].to_i if m[2]
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        return true
      end

      # iso25: YYYY-DDD (guard against fraction match)
      unless str =~ /[,.]\d{2,4}-\d{3}\b/
        if (m = PARSE_ISO25_RE.match(str))
          h[:year] = m[1].to_i
          h[:yday] = m[2].to_i
          str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
          return true
        end
      end

      # iso26: -DDD (guard against digit-DDD)
      unless str =~ /\d-\d{3}\b/
        if (m = PARSE_ISO26_RE.match(str))
          h[:yday] = m[1].to_i
          str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
          return true
        end
      end

      false
    end

    def parse_year(str, h)
      if (m = PARSE_YEAR_RE.match(str))
        h[:year] = m[1].to_i
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_mon(str, h)
      if (m = PARSE_MON_RE.match(str))
        mon = ABBR_MONTH_NUM[m[1].downcase]
        if mon
          h[:mon] = mon
          str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
          return true
        end
      end
      false
    end

    def parse_mday(str, h)
      if (m = PARSE_MDAY_RE.match(str))
        h[:mday] = m[1].to_i
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_ddd(str, h)
      if (m = PARSE_DDD_RE.match(str))
        sign = m[1]
        s2 = m[2]
        s3 = m[3]
        s4 = m[4]
        s5 = m[5]

        l2 = s2.length
        case l2
        when 2
          if s3.nil? && s4
            h[:sec] = s2.to_i
          else
            h[:mday] = s2.to_i
          end
        when 4
          if s3.nil? && s4
            h[:sec] = s2[2, 2].to_i
            h[:min] = s2[0, 2].to_i
          else
            h[:mon]  = s2[0, 2].to_i
            h[:mday] = s2[2, 2].to_i
          end
        when 6
          if s3.nil? && s4
            h[:sec]  = s2[4, 2].to_i
            h[:min]  = s2[2, 2].to_i
            h[:hour] = s2[0, 2].to_i
          else
            h[:year] = (sign.to_s + s2[0, 2]).to_i
            h[:mon]  = s2[2, 2].to_i
            h[:mday] = s2[4, 2].to_i
          end
        when 8, 10, 12, 14
          if s3.nil? && s4
            # read from end: sec,min,hour,mday,mon,year
            pos = l2
            h[:sec]  = s2[pos - 2, 2].to_i
            pos -= 2
            h[:min]  = s2[pos - 2, 2].to_i
            pos -= 2
            h[:hour] = s2[pos - 2, 2].to_i
            pos -= 2
            if pos >= 2
              h[:mday] = s2[pos - 2, 2].to_i
              pos -= 2
              if pos >= 2
                h[:mon] = s2[pos - 2, 2].to_i
                pos -= 2
                h[:year] = (sign.to_s + s2[0, pos]).to_i if pos > 0
              end
            end
          else
            h[:year] = (sign.to_s + s2[0, 4]).to_i
            h[:mon]  = s2[4, 2].to_i  if l2 >= 6
            h[:mday] = s2[6, 2].to_i  if l2 >= 8
            h[:hour] = s2[8, 2].to_i  if l2 >= 10
            h[:min]  = s2[10, 2].to_i if l2 >= 12
            h[:sec]  = s2[12, 2].to_i if l2 >= 14
            h[:_comp] = false
          end
        when 3
          if s3.nil? && s4
            h[:sec] = s2[1, 2].to_i
            h[:min] = s2[0, 1].to_i
          else
            h[:yday] = s2.to_i
          end
        when 5
          if s3.nil? && s4
            h[:sec]  = s2[3, 2].to_i
            h[:min]  = s2[1, 2].to_i
            h[:hour] = s2[0, 1].to_i
          else
            h[:year] = (sign.to_s + s2[0, 2]).to_i
            h[:yday] = s2[2, 3].to_i
          end
        when 7
          if s3.nil? && s4
            h[:sec]  = s2[5, 2].to_i
            h[:min]  = s2[3, 2].to_i
            h[:hour] = s2[1, 2].to_i
            h[:mday] = s2[0, 1].to_i
          else
            h[:year] = (sign.to_s + s2[0, 4]).to_i
            h[:yday] = s2[4, 3].to_i
            h[:_comp] = false
          end
        end

        # s3 (time portion from continuous digits after separator)
        if s3 && !s3.empty?
          l3 = s3.length
          if s4
            # read from end
            case l3
            when 2
              h[:sec]  = s3[0, 2].to_i
            when 4
              h[:sec]  = s3[2, 2].to_i
              h[:min]  = s3[0, 2].to_i
            when 6
              h[:sec]  = s3[4, 2].to_i
              h[:min]  = s3[2, 2].to_i
              h[:hour] = s3[0, 2].to_i
            end
          else
            # read from start
            h[:hour] = s3[0, 2].to_i if l3 >= 2
            h[:min]  = s3[2, 2].to_i if l3 >= 4
            h[:sec]  = s3[4, 2].to_i if l3 >= 6
          end
        end

        # s4: sec_fraction
        if s4 && !s4.empty?
          h[:sec_fraction] = parse_sec_fraction(s4)
        end

        # s5: zone
        if s5 && !s5.empty?
          zone = s5
          if zone.start_with?('[')
            zone = zone[1...-1] # strip brackets
            # Format: [offset:zonename] or [offset zonename] or [offset] or [zonename]
            if (zm = zone.match(/\A([-+]?\d+(?:[,.]\d+)?):(.+)/))
              # +9:JST, -5:EST, +12:XXX YYY ZZZ
              h[:zone] = zm[2].strip
              off_s = zm[1]
              off_s = "+#{off_s}" unless off_s.start_with?('+') || off_s.start_with?('-')
              h[:offset] = fast_zone_offset(off_s)
            elsif (zm = zone.match(/\A([-+]?\d+(?:[,.]\d+)?)\s+(\S.+)/))
              # Number followed by space and non-empty name
              h[:zone] = zm[2]
              off_s = zm[1]
              off_s = "+#{off_s}" unless off_s.start_with?('+') || off_s.start_with?('-')
              h[:offset] = fast_zone_offset(off_s)
            else
              # Could be just a number with optional trailing space: [9], [-9], [9 ]
              h[:zone] = zone
              stripped = zone.strip
              if stripped =~ /\A([-+]?\d+(?:[,.]\d+)?)\z/
                off_s = $1
                off_s = "+#{off_s}" unless off_s.start_with?('+') || off_s.start_with?('-')
                h[:offset] = fast_zone_offset(off_s)
              end
            end
          else
            h[:zone] = zone
          end
        end

        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
        true
      else
        false
      end
    end

    def parse_bc_post(str, h)
      if (m = PARSE_BC_RE.match(str))
        h[:_bc] = true
        str[m.begin(0)...m.end(0)] = ' ' * (m.end(0) - m.begin(0))
      end
    end

    def parse_frag(str, h)
      if (m = PARSE_FRAG_RE.match(str))
        v = m[1].to_i
        if h.key?(:hour) && !h.key?(:mday)
          h[:mday] = v if v >= 1 && v <= 31
        elsif h.key?(:mday) && !h.key?(:hour)
          h[:hour] = v if v >= 0 && v <= 24
        end
      end
    end

    # s3e: 3-element (year, month, day) disambiguation
    # Faithfully mirrors the C implementation's s3e() logic.
    # Arguments: y, m, d are strings (or Integer for m when month name was parsed)
    def s3e(h, y, m, d, bc)
      # Fast path: y is unambiguously a year (3+ digits or signed), m and d are simple digits.
      # This covers ISO "2001-02-03" but not ambiguous cases like "23/5/1999".
      if y && m && d && !bc && y.is_a?(String) && m.is_a?(String) && d.is_a?(String) &&
         !y.start_with?("'") && !d.start_with?("'") &&
         y =~ /\A([-+])?\d{3,}\z/ && m =~ /\A\d+\z/ && d =~ /\A\d{1,2}\z/
        h[:year] = y.to_i
        h[:_comp] = false
        h[:mon] = m.to_i
        h[:mday] = d.to_i
        return
      end

      m = m.to_s if m.is_a?(Integer)

      # Step 1: If y && m are present but d is nil, rotate: d=m, m=y, y=nil
      if y && m && d.nil?
        d = m
        m = y
        y = nil
      end

      # Step 2: If y is nil but d is present, check if d looks like a year
      if y.nil? && d
        ds = d.to_s
        digits = ds.sub(/\A'?-?/, '').sub(/(?:st|nd|rd|th)\z/i, '')
        if digits.length > 2 || ds.start_with?("'")
          y = d
          d = nil
        end
      end

      # Step 3: Parse y - extract numeric value and determine _comp flag
      year_val = nil
      comp_flag = nil
      if y
        ys_raw = y.to_s
        if ys_raw.start_with?("'")
          year_val = ys_raw[1..].to_i
          comp_flag = true
        else
          # Match digits (with optional leading sign), check for trailing non-digit
          if ys_raw =~ /\A[^-+\d]*([-+]?\d+)/
            num_s = $1
            rest = ys_raw[$~.end(0)..]
            if rest && !rest.empty? && rest =~ /[^\d]/
              # trailing non-digit (like "st" in "1st"): this y becomes d, old d becomes y
              old_d = d
              d = num_s
              year_val = nil
              comp_flag = nil
              if old_d
                s3eparse_year(old_d.to_s)&.then do |v, c|
                  year_val = v
                  comp_flag = c
                end
              end
            else
              year_val = num_s.to_i
              if num_s.start_with?('-') || num_s.start_with?('+') || num_s.sub(/\A[-+]/, '').length > 2
                comp_flag = false
              end
            end
          end
        end
      end

      # Step 4: Check m - if it looks like a year (apostrophe or > 2 digits), swap US→BE
      if m.is_a?(String)
        ms_digits = m.sub(/\A'?-?/, '').sub(/(?:st|nd|rd|th)\z/i, '')
        if m.start_with?("'") || ms_digits.length > 2
          # Rotate: old_y=y, y=m, m=d, d=old_y_string
          old_y = y
          y = m
          m = d
          d = old_y

          # Re-parse y
          ys = y.to_s.sub(/(?:st|nd|rd|th)\z/i, '')
          if ys.start_with?("'")
            year_val = ys[1..].to_i
            comp_flag = true
          elsif ys =~ /([-+]?\d+)/
            num_s = $1
            year_val = num_s.to_i
            comp_flag = (num_s.start_with?('-') || num_s.start_with?('+') || num_s.sub(/\A[-+]/, '').length > 2) ? false : nil
          end
        end
      end

      # Step 5: Check d - if it looks like a year, swap with y
      if d.is_a?(String)
        ds_digits = d.sub(/\A'?-?/, '').sub(/(?:st|nd|rd|th)\z/i, '')
        if d.start_with?("'") || ds_digits.length > 2
          old_y = y
          # d becomes year
          ys = d.sub(/(?:st|nd|rd|th)\z/i, '')
          if ys.start_with?("'")
            year_val = ys[1..].to_i
            comp_flag = true
          elsif ys =~ /([-+]?\d+)/
            num_s = $1
            year_val = num_s.to_i
            comp_flag = (num_s.start_with?('-') || num_s.start_with?('+') || num_s.sub(/\A[-+]/, '').length > 2) ? false : nil
          end
          d = old_y
        end
      end

      # Set year
      h[:year] = year_val if year_val
      h[:_comp] = comp_flag unless comp_flag.nil?
      h[:_bc] = true if bc

      # Set mon
      if m
        if m.is_a?(String)
          ms = m.sub(/(?:st|nd|rd|th)\z/i, '')
          h[:mon] = $1.to_i if ms =~ /(\d+)/
        else
          h[:mon] = m.to_i
        end
      end

      # Set mday
      if d
        if d.is_a?(String)
          ds = d.sub(/(?:st|nd|rd|th)\z/i, '')
          h[:mday] = $1.to_i if ds =~ /(\d+)/
        else
          h[:mday] = d.to_i
        end
      end
    end

    # Helper: parse a string as a year value, returning [year_val, comp_flag] or nil
    def s3eparse_year(s)
      if s.start_with?("'")
        [s[1..].to_i, true]
      elsif s =~ /\A[^-+\d]*([-+]?\d+)/
        num_s = $1
        cf = (num_s.start_with?('-') || num_s.start_with?('+') || num_s.sub(/\A[-+]/, '').length > 2) ? false : nil
        [num_s.to_i, cf]
      end
    end

    # ------------------------------------------------------------------
    # Fast Date construction
    # ------------------------------------------------------------------

    # Fast Date construction: when year/mon/mday are all present and no
    # complex keys (jd, yday, cwyear, wday, wnum, seconds) exist, skip
    # the full _sp_complete_frags pipeline and directly validate civil date.
    def fast_new_date(hash, sg)
      raise Error, 'invalid date' if hash.nil? || hash.empty?
      y = hash[:year]
      m = hash[:mon]
      d = hash[:mday]
      if y && m && d &&
         !hash.key?(:jd) && !hash.key?(:yday) && !hash.key?(:cwyear) &&
         !hash.key?(:wnum0) && !hash.key?(:wnum1) && !hash.key?(:seconds)
        jd = internal_valid_civil?(y, m, d, sg)
        raise Error, 'invalid date' if jd.nil?
        new_from_jd(jd, sg)
      else
        internal_new_by_frags(hash, sg)
      end
    end

  end
end
