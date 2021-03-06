-- Copyright 2017 Fabricio Ribeiro Toloczko 

-- OpenGPU

-- -- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at

--     http://www.apache.org/licenses/LICENSE-2.0

-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.ogpu_data_record_pkg.all;

--TODO: improve this algorithm, optmize for speed

entity ogpu_quad_generator is
	port(clock: in std_logic;
		  reset: in std_logic;
		  d: in ogpu_quad_generator_in_type;
		  q: out ogpu_quad_generator_out_type);
end ogpu_quad_generator;

architecture generator_1 of ogpu_quad_generator is
	type reg_type is record
		next_quad: std_logic;
		end_tile: std_logic;
		quad_ready: std_logic;
		quad: ogpu_quad;
		generate_quads: std_logic;
		i,j,x0,x1,y1: unsigned(15 downto 0);
	end record;
	signal r,rin	:	reg_type;
begin
comb: process(reset,d,r)
	variable v : reg_type ;
	begin
		v := r; --default assignment
		v.next_quad := d.next_quad;
		
		if d.next_quad = '1' and r.next_quad = '0' then
		--v.end_tile := '0';
			if r.generate_quads='0' then
				v.end_tile := '0';
				v.i:=(others => '0');
				v.j:=(others => '0');
				v.x0:=(others => '0');
				v.x1:=(others => '0');
				v.y1:=(others => '0');
				-- tile is valid, so, generate quads
				v.generate_quads := '1';
				-- clip tile, discarding void areas
				if d.tile.y0 <= d.clip_rect.y0 then
					v.i := d.clip_rect.y0;
				else
					v.i := d.tile.y0;
				end if;
				
				if d.tile.x0 <= d.clip_rect.x0 then
					v.j := d.clip_rect.x0;
				else
					v.j := d.tile.x0;
				end if;
				v.x0 := v.j;
				
				if d.tile.x1 >= d.clip_rect.x1 then
					v.x1 := d.clip_rect.x1;
				else
					v.x1 := d.tile.x1;
				end if;
				
				if d.tile.y1 >= d.clip_rect.y1 then
					v.y1 := d.clip_rect.y1;
				else
					v.y1 := d.tile.y1;
				end if;
				-- after clipping, guarantee even number for i or x0
				-- this is for avoid quads starting with odd position
				
				--host is responsible for correct tile selection -- NO CLIPRECT OPTIMIZATION
--				v.i := d.tile.y0;
--				v.j := d.tile.x0;
--				v.x0 := d.tile.x0;
--				v.x1 := d.tile.x1;
--				v.y1 := d.tile.y1;
				
				--v.i(0) := '0';
				--v.x0(0) := '0';
			end if;
			v.quad.x0 := v.j;		v.quad.x1 := v.j+1;
			v.quad.y0 := v.i;		v.quad.y1 := v.i;
			v.quad.x2 := v.j;		v.quad.x3 := v.j+1;
			v.quad.y2 := v.i+1;	v.quad.y3 := v.i+1;
			
			v.quad_ready := '1';
			
			v.j := v.j+2;
			if v.j>v.x1 then
				v.j := v.x0;
				v.i := v.i+2;
				if v.i>v.y1 then
					v.end_tile := '1';
					v.generate_quads := '0';
				end if;
			end if;
			
		elsif d.next_quad = '0' then -- and r.next_quad = '1' then
			v.quad_ready := '0';
		end if;
		
		if (d.tile.x1 < d.clip_rect.x0) or (d.tile.x0 > d.clip_rect.x1) then
			v.quad_ready := '0';
			v.end_tile := '1';
		elsif (d.tile.y1 < d.clip_rect.y0) or (d.tile.y0 > d.clip_rect.y1) then
			v.quad_ready := '0';
			v.end_tile := '1';
		end if;
		
		if d.start_raster = '0' then
			v.end_tile := '0';
			v.quad_ready := '0';
		end if;
		
		if reset = '1' then
			v.quad_ready := '0';
			v.end_tile := '0';
			v.generate_quads := '0';
			v.i:=(others => '0');
			v.j:=(others => '0');
			v.x0:=(others => '0');
			v.x1:=(others => '0');
			v.y1:=(others => '0');
		end if;
		
		rin <= v;		-- drive register inputs
		
		--q.quad_ready <= v.quad_ready;  -- drive module outputs
		q.quad_ready <= r.quad_ready;  -- drive module outputs
		q.end_tile <= r.end_tile;
		q.quad <= v.quad;
		
	end process;
	
seq: process(clock)
	begin
		if rising_edge(clock) then r <= rin; end if;
	end process;
end generator_1;

architecture generator_2 of ogpu_quad_generator is -- functional, but generating more than one quad for each next_quad rising edge
	type reg_type is record
		next_quad: std_logic;
		end_tile: std_logic;
		quad_ready: std_logic;
		quad: ogpu_quad;
		generate_quads: std_logic;
		i,j,x0,x1,y1: unsigned(15 downto 0);
	end record;
	signal r,rin	:	reg_type;
begin
comb: process(reset,d,r)
	variable v : reg_type;
	begin
		v := r; --default assignment
		v.next_quad := d.next_quad;
		v.quad_ready := '0';
		
		if d.next_quad = '1' and r.next_quad = '0' then
			v.end_tile := '0';
		elsif r.next_quad='1' then
			if r.generate_quads='0' and r.end_tile = '0' then
				v.end_tile := '0';
				v.i:=(others => '0');
				v.j:=(others => '0');
				v.x0:=(others => '0');
				v.x1:=(others => '0');
				v.y1:=(others => '0');
				if (d.tile.x1 < d.clip_rect.x0) or (d.tile.x0 > d.clip_rect.x1) then
					v.quad_ready := '0';
					v.end_tile := '1';
				else
					if (d.tile.y1 < d.clip_rect.y0) or (d.tile.y0 > d.clip_rect.y1) then
						v.quad_ready := '0';
						v.end_tile := '1';
					else
						-- tile is valid, so, generate quads
						v.generate_quads := '1';
						-- clip tile, discarding void areas
						if d.tile.y0 <= d.clip_rect.y0 then
							v.i := d.clip_rect.y0;
						else
							v.i := d.tile.y0;
						end if;
						
						if d.tile.x0 <= d.clip_rect.x0 then
							v.j := d.clip_rect.x0;
						else
							v.j := d.tile.x0;
						end if;
						v.x0 := v.j;
						
						if d.tile.x1 >= d.clip_rect.x1 then
							v.x1 := d.clip_rect.x1;
						else
							v.x1 := d.tile.x1;
						end if;
						
						if d.tile.y1 >= d.clip_rect.y1 then
							v.y1 := d.clip_rect.y1;
						else
							v.y1 := d.tile.y1;
						end if;
						-- after clipping, guarantee even number for i or x0
						-- this is for avoid quads starting with odd position
						v.i(0) := '0';
						v.x0(0) := '0';
					end if;
				end if;
			elsif r.generate_quads='1' and r.end_tile = '0' then
				v.quad.x0 := v.j;		v.quad.x1 := v.j+1;
				v.quad.y0 := v.i;		v.quad.y1 := v.i;
				v.quad.x2 := v.j;		v.quad.x3 := v.j+1;
				v.quad.y2 := v.i+1;	v.quad.y3 := v.i+1;
				
				v.j := v.j+2;
				if v.j>v.x1 then
					v.j := v.x0;
					v.i := v.i+2;
					if v.i>v.y1 then
						v.end_tile := '1';
						v.generate_quads := '0';
					end if;
				end if;
				v.quad_ready := '1';
			end if;
		else
			v.quad_ready := '0';
		end if;
		
		if reset = '1' then
			v.quad_ready := '0';
			v.end_tile := '0';
			v.generate_quads := '0';
			v.i:=(others => '0');
			v.j:=(others => '0');
			v.x0:=(others => '0');
			v.x1:=(others => '0');
			v.y1:=(others => '0');
		end if;
		
		rin <= v;		-- drive register inputs
		
		--q.quad_ready <= v.quad_ready;  -- drive module outputs
		q.quad_ready <= r.quad_ready;  -- drive module outputs
		q.end_tile <= v.end_tile;
		q.quad <= v.quad;
		
	end process;
	
seq: process(clock)
	begin
		if rising_edge(clock) then r <= rin; end if;
	end process;
end generator_2;