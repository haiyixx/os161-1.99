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
bool check_constraints(Vehicle *newVehicle, Vehicle *currVehicle);
bool able_to_enter(Direction origin, Direction destination);

static struct lock *arrayLock;
static struct cv *meetCondition;
static struct array *vehicles;



bool
right_turn(Vehicle *v) {
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
check_constraints(Vehicle *newVehicle, Vehicle *currVehicle)
{
  if (newVehicle->origin == currVehicle->origin) return true;

  /* no conflict if vehicles go in opposite directions */
  if ((newVehicle->origin == currVehicle->destination) &&
    (newVehicle->destination == currVehicle->origin)) return true;

  /* no conflict if one makes a right turn and
    the other has a different destination */
  if ((right_turn(newVehicle) || right_turn(currVehicle)) &&
    (newVehicle->destination != currVehicle->destination)) return true;

  return false;
}

bool
able_to_enter(Direction origin, Direction destination)
{

  Vehicle *newVehicle = kmalloc(sizeof(struct Vehicles));
  newVehicle->origin = origin;
  newVehicle->destination = destination;

  int num = array_num(vehicles);
  for(int i=0; i<num; i++) {
    if (!check_constraints(newVehicle, array_get(vehicles, i))){
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

  arrayLock = lock_create("arrayLock");
  if (arrayLock == NULL) {
    panic("could not create arrray lock");
  }

  meetCondition = cv_create("meetCondition");
  if (meetCondition == NULL) {
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
  cv_destroy(meetCondition);
  lock_destroy(arrayLock);
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
  KASSERT(arrayLock != NULL);
  KASSERT(meetCondition != NULL);

  lock_acquire(arrayLock);
  // if does not meet condition, sleep on wait chanel
  while (!able_to_enter(origin, destination)){
    cv_wait(meetCondition, arrayLock);
  }

  //create new vehicle, add to vehicle array
  Vehicle *newVehicle = kmalloc(sizeof(struct Vehicles));
  newVehicle->origin = origin;
  newVehicle->destination = destination;
  array_add(vehicles, newVehicle, NULL);
  lock_release(arrayLock);

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
  KASSERT(arrayLock != NULL);
  KASSERT(meetCondition != NULL);

  lock_acquire(arrayLock);
  int num = array_num(vehicles);

  for(int i=0; i<num; i++) {
    Vehicle *temp = array_get(vehicles, i);
    if (origin == temp->origin && destination == temp->destination) {
     array_remove(vehicles, i);
     cv_broadcast(meetCondition, arrayLock);
     lock_release(arrayLock);
     return;
   }
  }
  return;
}
