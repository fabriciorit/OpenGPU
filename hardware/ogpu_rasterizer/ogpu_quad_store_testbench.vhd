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

entity ogpu_quad_store_testbench is
end ogpu_quad_store_testbench;

architecture ogpu_quad_store_testbench_tb1 of ogpu_quad_store_testbench is
	constant clock_period : time := 20 ns;
	signal clock,reset	: std_logic :='0';
	--
	signal quad_mask: std_logic_vector(0 to 3);
	signal quad: ogpu_quad;
	signal depth_quad: ogpu_depth_quad;

	signal buffer_ack: std_logic:='0';
	signal addr: std_logic_vector(63 downto 0);

	signal quad_buffer_length: std_logic_vector(23 downto 0);
	signal buffer_address: std_logic_vector(15 downto 0);
	signal buffer_byte_enable: std_logic_vector(7 downto 0);
	signal buffer_write: std_logic:='0';
	signal buffer_write_data: std_logic_vector(63 downto 0);
	
	-- DATAPATH/CONTROL interface signals
	signal start_raster: std_logic:='0';
	signal store_quad: std_logic:='0';
	signal quad_stored: std_logic:='0';
	
	--internal testbench signals
	signal run_proc: std_logic :='0';

begin
QS1:  entity work.ogpu_quad_store(quad_store_1) port map(
									-- IN
									clock=>clock,reset=>reset,
									d.addr=>addr,
									d.quad_mask=>quad_mask,
									d.quad=>quad,
									d.depth_quad=>depth_quad,
									d.start_raster=>start_raster,
									d.store_quad=>store_quad,
									d.buffer_ack=>buffer_ack,
									-- OUT
									q.quad_stored=>quad_stored,
									q.quad_buffer_length=>quad_buffer_length,
									q.buffer_address=>buffer_address,
									q.buffer_byte_enable=>buffer_byte_enable,
									q.buffer_write=>buffer_write,
									q.buffer_write_data=>buffer_write_data);							

reset_proc: process
	begin
		--reset <= '0';
		--wait for 5*clock_period;
		reset <= '1';
		wait for 5*clock_period;
		reset <= '0';
		wait;
	end process;
	
ext_memory_proc: process
	begin
		wait until buffer_write = '1';
		wait until falling_edge(clock) and clock = '0';
		wait for 3*clock_period;
		buffer_ack <= '1';
		wait for clock_period;
		buffer_ack <= '0';
	end process;
	
init_stim_proc: process
	begin
		wait until reset = '0';
		depth_quad <= (others=>(others=>'0'));
		addr<=(others=>'0');
		run_proc<='1';
		wait;
	end process;
	
stim_proc: process
	variable i:integer:=0;
	variable j:integer:=0;
	begin
		if run_proc = '0' then
			wait until run_proc = '1';
			start_raster <= '1';
			wait for 3*clock_period;
		end if;
		quad_mask<=std_logic_vector(to_unsigned(i,4));
		quad.x0<=to_unsigned(i,16);	quad.x1<=to_unsigned(i+1,16);
		quad.y0<=to_unsigned(j,16);	quad.y1<=to_unsigned(j,16);
		quad.x2<=to_unsigned(i,16);	quad.x3<=to_unsigned(i+1,16);
		quad.y2<=to_unsigned(j+1,16);	quad.y3<=to_unsigned(j+1,16);
		
		store_quad <= '1';
		wait until quad_stored = '1';
		store_quad <= '0';
		i:=i+2;
		if i>63 then
			i:=0;
			j:=j+2;
			if j>63 then
				j:=0;
				start_raster<='0';
				wait for 3*clock_period;
				start_raster<='1';
			end if;
		end if;
		wait for 2*clock_period;
	end process;
								
clock_process: process
	begin
		clock <= not clock;
		wait for clock_period/2;
	end process;
end ogpu_quad_store_testbench_tb1;