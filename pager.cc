#include <cstdlib>
#include <assert.h>
#include <iostream>
#include <stdio.h>
#include <ctype.h>
#include <map>
#include <vector>
#include <deque>
#include <queue>
#include <ucontext.h>
#include <iterator>
#include "vm_pager.h"
#include <stdlib.h>
#include <string.h>

using namespace std;

void vm_init(unsigned int memory_pages, unsigned int disk_blocks);
void vm_create(pid_t pid);
void vm_switch(pid_t pid);
int vm_fault(void *addr, bool write_flag);
void vm_destroy();
void * vm_extend();
int vm_syslog(void *message, unsigned int len);

typedef struct 
{
	bool dirty_bit;
	bool reference_bit;
	bool zero_bit;
	bool resident;
	int vp_num;
	pid_t process_num;
	int block_index;
} page_t;

typedef struct 
{
	pid_t pros_id;
	page_table_t *pt_pointer;
	vector<page_t*> pages;
	unsigned long int valid_addr;
	
} process_t;

queue<page_t*> clockq;
map<pid_t, process_t*> process_map; 
queue<int> mem_blocks;
queue<int> free_pages;

pid_t curr_process = 0;
unsigned int ppage_entry_num;

unsigned long int find_lowest_addr()
{
	unsigned long int lowest_addr;
	unsigned long int arena_base = (unsigned long int) VM_ARENA_BASEADDR;
	unsigned int page_size = (unsigned int) VM_PAGESIZE;


	if(process_map[curr_process]->valid_addr == arena_base)
	{
		lowest_addr = arena_base;
	}
	else
	{
		lowest_addr = process_map[curr_process]->valid_addr+1;
	}

	return lowest_addr;
}

void vm_init(unsigned int memory_pages, unsigned int disk_blocks)
{

	unsigned int arena_size = (unsigned int) VM_ARENA_SIZE;
	unsigned int page_size = (unsigned int) VM_PAGESIZE;

	for(int i = 0; i < disk_blocks; i++)
	{
		mem_blocks.push(i);
	}

	for(int j = 0; j < memory_pages; j++)
	{
		free_pages.push(j);
	}

	ppage_entry_num = (arena_size/page_size) + 1;

}

void vm_create(pid_t pid)
{
	unsigned int arena_size = (unsigned int) VM_ARENA_SIZE;
	unsigned int page_size = (unsigned int) VM_PAGESIZE;
	unsigned long int arena_base = (unsigned long int) VM_ARENA_BASEADDR;
	unsigned int total_page_table_ts = (arena_size/page_size);
	


	process_t* new_proc = new process_t;
	new_proc->pros_id = pid;
	new_proc->pt_pointer = new page_table_t;


	page_table_entry_t* new_pte = new page_table_entry_t;



	for(int i = 0; i < total_page_table_ts; i++)
	{
		new_pte = &new_proc->pt_pointer->ptes[i];
		new_pte->read_enable = 0;
		new_pte->write_enable = 0;
		new_pte->ppage = ppage_entry_num+1;
	}

	new_proc->valid_addr = arena_base;
	process_map.insert(pair<pid_t, process_t*>(new_proc->pros_id, new_proc));

	
}

/*pager determines which accesses in arena will fault with read_enable, write_enable fields in page table
determine which physical page -> virtual page with ppage field in page table
physical page associated with at most one virtual page in one process at any given time
should (return 0) after successful handling, (return -1) if address to invalid page 
*/
int vm_fault(void *addr, bool write_flag)
{
	unsigned long int virtual_addr = (unsigned long int) addr;
	unsigned long int arena_base = (unsigned long int) VM_ARENA_BASEADDR;
	unsigned int page_size = (unsigned int) VM_PAGESIZE; 

	//find index in page table of (addr)
	unsigned int virtual_addr_pt_index = (int)((virtual_addr - arena_base)/page_size);

	//if the address being passed in is the current process's address space or if its less than the arena base address -- return -1
	if((process_map[curr_process]->valid_addr < virtual_addr) || (arena_base > virtual_addr))
	{
		return -1;
	}

	//create new entry based on that page table index
	page_table_entry_t *entry = &process_map[curr_process]->pt_pointer->ptes[virtual_addr_pt_index];

	page_t *faulting_page = process_map[curr_process]->pages[virtual_addr_pt_index];

	//check if page at that index is resident -- if it is create a new reference to that resident page at the index calculated


	
	if(faulting_page->resident == true) //page is resident 
	{
		
		//printf("page is resident\n");

		//if write flag is set high -- check if page has been changed to
		//if it has been written to set that entry to write enable true
		//else, page hasn't been changed set write enable to 1, zero bit, and dirty bit
		//page has now been referenced and can be read
		if(write_flag == false)
		{
			if(faulting_page->dirty_bit)
			{	
				//printf("resident page dirty\n");
				entry->write_enable = 1;
			}
		}
		else
		{
			entry->write_enable = 1;
			faulting_page->zero_bit = true;
			faulting_page->dirty_bit = true;

		}

		faulting_page->reference_bit = true;
		entry->read_enable = 1;
	}

	//page at index is not resident -- create reference to that page to add to clock queue if there is space
	else
	{

		//printf("page is not resident\n");
		//page_t *page_not_resident = process_map[curr_process]->pages[virtual_addr_pt_index];

		//do not need to evict any pages
		if(free_pages.size() >= 1)
		{
			//printf("Dont need to evict\n");
			//entry is the first free page -- remove a free page from the list: we have one less now
			entry->ppage = free_pages.front();
			free_pages.pop();

			//need to read a block from disk to physical memory only if its not already there
			//if it is set that block in physical memory
			if(faulting_page->zero_bit)
			{
				disk_read(((unsigned int)faulting_page->block_index), ((unsigned int)(process_map[curr_process]->pt_pointer->ptes[virtual_addr_pt_index]).ppage));
			}
			else
			{
				unsigned int mem_block = page_size * process_map[curr_process]->pt_pointer->ptes[virtual_addr_pt_index].ppage;
				memset((char*)pm_physmem + mem_block, 0, page_size);
			}

			//if fault was called by a write need to set correct bits of page to true
			//and enable stores for that entry

			//else set the resident and reference bits to true for that page
			//because we have made it resident and have touched that page now
			//and enable loads for that entry
			if(write_flag == true)
			{
				faulting_page->zero_bit = true;
				faulting_page->dirty_bit = true;
				entry->write_enable = 1;
				//printf("page not evicted: write flag set \n");
			}

			faulting_page->resident = true;
			faulting_page->reference_bit = true;
			entry->read_enable = 1;

			clockq.push(faulting_page);

		}

		//need to evict a page

		else
		{


			bool evict = false;

			//while a page to evicted hasnt been found get first thing in clock queue
			//entry is that pages page table entry
			//if that page has been reference recently then it cannot be evicted -- push it back on queue
			//if it hasnt been referenced recently then you found page and assocaited entry and we can exit loop

			while(!evict)
			{
				//get references to a temp entry and the pages in the clock q
				page_table_entry_t *temp_entry;
				page_t *page;



				page = clockq.front();
				clockq.pop();

				temp_entry = &process_map[curr_process]->pt_pointer->ptes[page->vp_num];

				
				if(page->reference_bit == true)
				{
					page->reference_bit = false;
					temp_entry->write_enable = 0;
					temp_entry->read_enable = 0;
					clockq.push(page);
					//printf("reference_bit %d\n", page->reference_bit);

				}
				else
				{

					page->resident = false;

					//if our candidate page to evict has been modified -- reset it and write it to disk at its block index
					//if it hasnt been modified set our temp entry's ppage to our current process's page table entry ppage
					//our evicted page is no longer resident after it has been written to disk
					if(page->dirty_bit == true)
					{
						//printf("Disk write\n");
						disk_write(page->block_index, temp_entry->ppage);
						page->dirty_bit = false;
				
					}

					entry->ppage = temp_entry->ppage;
					//need to read a block from disk to physical memory only if its not already there
					//if it is set that block in physical memory
					if(faulting_page->zero_bit == true)
					{
					     disk_read(((unsigned int)faulting_page->block_index),((unsigned int)(process_map[curr_process]->pt_pointer->ptes[virtual_addr_pt_index]).ppage));
					}
					else
					{
						unsigned int mem_block = page_size * process_map[curr_process]->pt_pointer->ptes[virtual_addr_pt_index].ppage;
						memset((char*)pm_physmem + mem_block, 0, page_size);
					}

					//if fault was called by a write need to set correct bits of page to true
					//and enable stores for that entry

					//else set the resident and reference bits to true for that page
					//because we have made it resident and have touched that page now
					//and enable loads for that entry
					if(write_flag == true)
					{

						faulting_page->zero_bit = true;
						faulting_page->dirty_bit = true;
						entry->write_enable = 1;
						//printf("page not evicted: write flag set \n");

					}

					entry->read_enable = 1;
					temp_entry->ppage = ppage_entry_num + 1;

					faulting_page->resident = true;
					faulting_page->reference_bit = true;
					

					clockq.push(faulting_page);

					evict = true;
				}

			}

		}


		
	}

	
	return 0;

}


void vm_switch(pid_t pid)
{
	curr_process = pid;
	page_table_base_register = process_map[pid]->pt_pointer;
}

void *vm_extend()
{
	unsigned long int byte;
	unsigned long int *byte_pointer;
	unsigned int arena_size = (unsigned int) VM_ARENA_SIZE;
	unsigned long int arena_base = (unsigned long int) VM_ARENA_BASEADDR;
	unsigned int page_size = (unsigned int) VM_PAGESIZE;

	//if no more blocks -- cant extend 
	if(mem_blocks.size() <= 0)
	{
		return NULL;
	}

	//get first free block and remove it from list of free blocks
	unsigned int free_disk_block = mem_blocks.front();
	mem_blocks.pop();

	//find the lowest address 
	byte = find_lowest_addr();

	page_t *new_vp = new page_t;

	new_vp->dirty_bit = false;
	new_vp->reference_bit = false;
	new_vp->zero_bit = false;
	new_vp->resident = false;
	new_vp->process_num = curr_process;
	new_vp->block_index = free_disk_block;
	new_vp->vp_num = (byte - arena_base) / page_size;

	process_map[curr_process]->pages.push_back(new_vp);

	process_map[curr_process]->valid_addr = byte + page_size - 1;

	byte_pointer = (unsigned long int*)byte;


	return ((void *)byte_pointer);

}

/* must deallocate all resources held by that process: page tables, physical pages, disk blocks
physical pages that are released should be put back on free list */

void vm_destroy()
{
	//Go through clock queue and free all the process virtual page's corresponding physical pages
	//delete process's page table 
	//free all the process's valid virtual page space 
	//delete process


	page_t *page = new page_t;

	for(int i = 0; i < clockq.size(); i++)
	{
		//printf("vm_destroy: Iteraing through clock queue\n");
		page = clockq.front();
		clockq.pop();
		

		if(page->process_num == curr_process)
		{
			free_pages.push(process_map[curr_process]->pt_pointer->ptes[(unsigned int) page->vp_num].ppage);
		}
		else
		{
			clockq.push(page);
		}
		
		
	}

	//printf("vm_destroy: deleting current process's page table\n");
	delete process_map[curr_process]->pt_pointer; 


	for(int i = 0; i < process_map[page->process_num]->pages.size(); i++)
	{
		mem_blocks.push(process_map[curr_process]->pages[i]->block_index);
		delete process_map[curr_process]->pages[i];

	}
	
	//printf("vm_destroy: deleting current process\n");
	delete process_map[curr_process];
	//printf("vm_destroy: done\n");

}



int vm_syslog(void *message, unsigned int len)
{
	//initialize virtual address from message and string to print out
	unsigned long int virtual_addr = (unsigned long int)message;
	string pager_string;
	unsigned long int arena_base = (unsigned long int) VM_ARENA_BASEADDR;
	unsigned int page_size = (unsigned int) VM_PAGESIZE;

	

	//len has size zero: not valid string
	//if the message is bigger than the current process's valid address space 
	//or the virtual address of the message is not in Arena space: not valid string
	if(((virtual_addr + len - 1) > process_map[curr_process]->valid_addr) || (arena_base > virtual_addr))
	{
		return -1;
	}
	

	//Need to check if entire message can fit in pager's address space
	//if page table entry corresponding to virtual address of message is not read enabled 
	//or address of message is not a resident page (aka fault): return -1

	//Otherwise find the physical page number and the index corresponding to
	//message virtual address and copy to string in physical memory
	for (unsigned int i = 0; i < len; i++)
	{
		int page_index = (int)((virtual_addr - arena_base)/ page_size);
		int page_offset = (int)((virtual_addr - arena_base) % page_size);
		
		if(process_map[curr_process]->pt_pointer->ptes[page_index].read_enable == 0)
		{
			if (vm_fault((void *)virtual_addr, false) == -1)
			{
		
				return -1;
			}
		}

		unsigned int physical_page_num = (unsigned int)(process_map[curr_process]->pt_pointer->ptes[page_index].ppage);

		pager_string += ((char*)pm_physmem)[(int)(physical_page_num * page_size) + page_offset];

		virtual_addr += 1;

	}
		
	cout << "syslog \t\t\t" << pager_string << endl;

	return 0;

}




