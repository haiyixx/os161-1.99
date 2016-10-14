#include <types.h>
#include <lib.h>
#include <synchprobs.h>
#include <synch.h>
#include <opt-A1.h>
#include <array.h>
/*
 * This simple default synchronization mechanism allows only vehicle at a time
 * into the intersection.   The intersectionSem is used as a a lock.
 * We use a semaphore rather than a lock so that this code will work even
 * before locks are implemented.
 */

/*
 * Replace this default synchronization mechanism with your own (better) mechanism
 * needed for your solution.   Your mechanism may use any of the available synchronzation
 * primitives, e.g., semaphores, locks, condition variables.   You are also free to
 * declare other global variables if your solution requires them.
 */

/*
 * replace this with declarations of any synchronization and other variables you need here
 */

/*custom defined and forward declaration*/
typedef struct Vehicles
{
  Direction origin;
  Direction destination;
} Vehicle;

bool right_turn(Vehicle *v);
bool check_constraints(Vehicle *new_vehicle, Vehicle *curr_vehicle);
bool able_to_enter(Vehicle *new_vehicle);
void sleep_to_channel(Direction origin);
void wake_from_channel(Direction origin, Direction destination);
void random_broad_direction(struct cv *dir1, struct cv *dir2, struct cv *dir3);

static struct lock *array_lock;
static struct array *vehicles;
static struct cv *from_east, *from_west, *from_north, *from_south;

void
sleep_to_channel(Direction origin)
{
  if (origin == 0) {
    cv_wait(from_north, array_lock);
  } else if (origin == 1) {
    cv_wait(from_east, array_lock);
  } else if (origin == 2) {
    cv_wait(from_south, array_lock);
  } else {
    cv_wait(from_west, array_lock);
  }

}

void random_broad_direction(struct cv *dir1, struct cv *dir2, struct cv *dir3)
{
  int i = random() % 2;
  if (i == 0) {
    cv_broadcast(dir1, array_lock);
    cv_broadcast(dir2, array_lock);
    cv_broadcast(dir3, array_lock);
  } else {
    cv_broadcast(dir2, array_lock);
    cv_broadcast(dir1, array_lock);
    cv_broadcast(dir3, array_lock);
  }
}

void
wake_from_channel(Direction origin, Direction destination)
{
  if (origin == 0) {
      if (destination == 1) {
        random_broad_direction(from_south, from_west, from_east);
      } else if (destination == 2) {
        random_broad_direction(from_west, from_east, from_south);
      } else {
        random_broad_direction(from_east, from_south, from_west);
      }
  } else if (origin == 1) {
      if (destination == 0) {
        random_broad_direction(from_south, from_west, from_north);
      } else if (destination == 2) {
        random_broad_direction(from_west, from_north, from_south);
      } else {
        random_broad_direction(from_north, from_south, from_west);
      }
  } else if (origin == 2) {
      if (destination == 0) {
        random_broad_direction(from_east, from_west, from_north);
      } else if (destination == 1) {
        random_broad_direction(from_west, from_north, from_east);
      } else {
        random_broad_direction(from_east, from_north, from_west);
      }
  } else {
      if (destination == 0) {
        random_broad_direction(from_east, from_south, from_north);
      } else if (destination == 1) {
        random_broad_direction(from_south, from_north, from_east);
      } else {
        random_broad_direction(from_east, from_north, from_south);
      }
  }
}


bool
right_turn(Vehicle *v)
{
  KASSERT(v != NULL);
  if (((v->origin == west) && (v->destination == south)) ||
      ((v->origin == south) && (v->destination == east)) ||
      ((v->origin == east) && (v->destination == north)) ||
      ((v->origin == north) && (v->destination == west))) {
    return true;
  } else {
    return false;
  }
}

bool
check_constraints(Vehicle *new_vehicle, Vehicle *curr_vehicle)
{
  if (new_vehicle->origin == curr_vehicle->origin) return true;

  /* no conflict if vehicles go in opposite directions */
  if ((new_vehicle->origin == curr_vehicle->destination) &&
    (new_vehicle->destination == curr_vehicle->origin)) return true;

  /* no conflict if one makes a right turn and
    the other has a different destination */
  if ((right_turn(new_vehicle) || right_turn(curr_vehicle)) &&
    (new_vehicle->destination != curr_vehicle->destination)) return true;

  return false;
}

bool
able_to_enter(Vehicle *new_vehicle)
{
  int num = array_num(vehicles);
  for(int i=0; i<num; i++) {
    if (!check_constraints(new_vehicle, array_get(vehicles, i))) {
      return false;
    }
  }
  return true;
}

/*
 * The simulation driver will call this function once before starting
 * the simulation
 *
 * You can use it to initialize synchronization and other variables.
 *
 */
void
intersection_sync_init(void)
{

  array_lock = lock_create("array_lock");
  if (array_lock == NULL) {
    panic("could not create arrray lock");
  }

  from_north = cv_create("from_north");
  from_south = cv_create("from_south");
  from_east = cv_create("from_east");
  from_west = cv_create("from_west");
  if (from_north == NULL || from_south == NULL || from_east == NULL || from_west == NULL) {
    panic("could not create condition variable");
  }
  vehicles = array_create();
  if (vehicles == NULL) {
    panic("could not create vehicles array");
  }

  return;
}


/*
 * The simulation driver will call this function once after
 * the simulation has finished
 *
 * You can use it to clean up any synchronization and other variables.
 *
 */
void
intersection_sync_cleanup(void)
{
  cv_destroy(from_west);
  cv_destroy(from_south);
  cv_destroy(from_east);
  cv_destroy(from_north);
  lock_destroy(array_lock);
  array_destroy(vehicles);
}


/*
 * The simulation driver will call this function each time a vehicle
 * tries to enter the intersection, before it enters.
 * This function should cause the calling simulation thread
 * to block until it is OK for the vehicle to enter the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle is arriving
 *    * destination: the Direction in which the vehicle is trying to go
 *
 * return value: none
 */

void
intersection_before_entry(Direction origin, Direction destination)
{
  KASSERT(array_lock != NULL);
  KASSERT(from_east && from_west && from_north && from_south);

  lock_acquire(array_lock);

  //create new vehicle
  Vehicle *new_vehicle = kmalloc(sizeof(struct Vehicles));
  new_vehicle->origin = origin;
  new_vehicle->destination = destination;
  // if does not meet condition, sleep on wait chanel
  while (!able_to_enter(new_vehicle)) {
    sleep_to_channel(origin);
  }

  //add to vehicle array
  array_add(vehicles, new_vehicle, NULL);
  lock_release(array_lock);

}


/*
 * The simulation driver will call this function each time a vehicle
 * leaves the intersection.
 *
 * parameters:
 *    * origin: the Direction from which the vehicle arrived
 *    * destination: the Direction in which the vehicle is going
 *
 * return value: none
 */

void
intersection_after_exit(Direction origin, Direction destination)
{
  KASSERT(array_lock != NULL);
  KASSERT(from_east && from_west && from_north && from_south);

  lock_acquire(array_lock);
  int num = array_num(vehicles);

  for(int i=0; i<num; i++) {
    Vehicle *temp = array_get(vehicles, i);
    if (origin == temp->origin && destination == temp->destination) {
     array_remove(vehicles, i);
     wake_from_channel(origin, destination);
     lock_release(array_lock);
     return;
   }
  }
  return;
}
