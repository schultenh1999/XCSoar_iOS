// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The XCSoar Project

#ifdef __APPLE__

#include "Apple/InternalSensors.hpp"
#include "Device/SensorListener.hpp"
#include "Geo/GeoPoint.hpp"
#include "time/FloatDuration.hxx"
#include "time/SystemClock.hxx"

#include <TargetConditionals.h>

#include <unistd.h>

@implementation LocationDelegate
-(instancetype) init: (SensorListener *) _listener
{
  self = [super init];
  if (self) {
    self->listener = _listener;
    gregorian_calendar = [[NSCalendar alloc]
      initWithCalendarIdentifier:NSCalendarIdentifierGregorian];
#if TARGET_OS_IPHONE
    background_task = UIBackgroundTaskInvalid;
    
    // Register for background/foreground notifications
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(applicationDidEnterBackground:)
                                                 name:UIApplicationDidEnterBackgroundNotification
                                               object:nil];
    
    [[NSNotificationCenter defaultCenter] addObserver:self
                                             selector:@selector(applicationWillEnterForeground:)
                                                 name:UIApplicationWillEnterForegroundNotification
                                               object:nil];
#endif
  }
  return self;
}

-(double) getSecondsOfDay: (NSDate*) date
{
  NSDateComponents *components = [gregorian_calendar
    components: NSIntegerMax
    fromDate: date];
  [components setHour:0];
  [components setMinute:0];
  [components setSecond:0];
  NSDate *midnight = [gregorian_calendar dateFromComponents:components];
  return [date timeIntervalSinceDate: midnight];
}

#if TARGET_OS_IPHONE
- (void) handleAuthorizationStatus:(CLAuthorizationStatus)status
                           manager:(CLLocationManager *)manager
{
  switch (status) {
  case kCLAuthorizationStatusAuthorizedAlways:
    // "Always" permission: enable background updates, then start.
    if (@available(iOS 9.0, *)) {
      manager.allowsBackgroundLocationUpdates = YES;
    }
    if (@available(iOS 11.0, *)) {
      manager.showsBackgroundLocationIndicator = NO;
    }
    [manager startUpdatingLocation];
    break;

  case kCLAuthorizationStatusAuthorizedWhenInUse:
    /* "When In Use" permission: start updates so the GPS works in the
       foreground.  startUpdatingLocation must be called unconditionally
       here -- requestAlwaysAuthorization only prompts once and does not
       change the status (nor fire this delegate) if the user keeps "While
       Using", so relying on the callback to start updates would leave the
       GPS dead.  Background updates need "Always", so request the
       upgrade as well. */
    [manager startUpdatingLocation];
    if (@available(iOS 11.0, *)) {
      [manager requestAlwaysAuthorization];
    }
    break;

  case kCLAuthorizationStatusNotDetermined:
    // First time: request "When In Use" first.
    [manager requestWhenInUseAuthorization];
    break;

  default:
    /* Denied or restricted: still start updates so the GPS recovers
       automatically if the user later enables location in Settings. */
    [manager startUpdatingLocation];
    break;
  }
}

- (void) locationManager:(CLLocationManager *)manager
    didChangeAuthorizationStatus:(CLAuthorizationStatus)status
{
  // Deprecated since iOS 14, but still required for iOS < 14.
  [self handleAuthorizationStatus:status manager:manager];
}

- (void) locationManagerDidChangeAuthorization:(CLLocationManager *)manager
    API_AVAILABLE(ios(14.0))
{
  // iOS 14+ replacement for the deprecated callback above.
  [self handleAuthorizationStatus:manager.authorizationStatus manager:manager];
}
#endif

-(void) locationManager:(CLLocationManager *)manager
    didUpdateLocations:(NSArray *)locations
{
  CLLocation *location;
  if (locations)
    location = locations.lastObject;
  else
    location = nil;

  if (!location || !location.timestamp || location.horizontalAccuracy < 0.0) {
    self->listener->OnConnected(1);
    return;
  }

  const auto time = TimePointAfterUnixEpoch(FloatDuration{[location.timestamp timeIntervalSince1970]});

  const GeoPoint l(Angle::Degrees(location.coordinate.longitude),
                   Angle::Degrees(location.coordinate.latitude));

  self->listener->OnConnected(2);
  self->listener->OnLocationSensor(time, -1, l,
                                   location.verticalAccuracy >= 0.0,
                                   /* CoreLocation provides geoidal
                                      altitude */
                                   true,
                                   location.altitude,
                                   location.course >= 0.0,
                                   location.course,
                                   location.speed >= 0.0,
                                   location.speed,
                                   true, location.horizontalAccuracy);
}

- (void)locationManager:(CLLocationManager *)manager
    didFailWithError:(NSError *)error
{
  self->listener->OnConnected(0);
}

#if TARGET_OS_IPHONE
- (void)applicationDidEnterBackground:(NSNotification *)notification
{
  // Start a background task to allow location updates to continue briefly
  background_task = [[UIApplication sharedApplication] 
    beginBackgroundTaskWithName:@"LocationUpdates" 
    expirationHandler:^{
      // Clean up when the background task expires
      if (background_task != UIBackgroundTaskInvalid) {
        [[UIApplication sharedApplication] endBackgroundTask:background_task];
        background_task = UIBackgroundTaskInvalid;
      }
    }];
}

- (void)applicationWillEnterForeground:(NSNotification *)notification
{
  // End the background task when returning to foreground
  if (background_task != UIBackgroundTaskInvalid) {
    [[UIApplication sharedApplication] endBackgroundTask:background_task];
    background_task = UIBackgroundTaskInvalid;
  }
}

- (void)dealloc
{
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  if (background_task != UIBackgroundTaskInvalid) {
    [[UIApplication sharedApplication] endBackgroundTask:background_task];
    background_task = UIBackgroundTaskInvalid;
  }
}
#endif

@end


InternalSensors::InternalSensors(SensorListener &_listener)
  :listener(_listener)
#if TARGET_OS_IPHONE
  , altimeter(nullptr)
  , motion_activity_manager(nullptr)
  , motion_activity_queue(nullptr)
#endif
{
  if ([NSThread isMainThread]) {
    Init();
  } else {
    dispatch_sync(dispatch_get_main_queue(), ^{
      Init();
    });
  }
}

InternalSensors::~InternalSensors()
{
  if ([NSThread isMainThread]) {
    Deinit();
  } else {
    dispatch_sync(dispatch_get_main_queue(), ^{
      Deinit();
    });
  }
}

/**
 * Initialize all available sensors and request necessary permissions.
 * 
 * Sets up:
 * - CoreLocation manager with high accuracy GPS
 * - Permission requests for location services  
 * - iOS barometric pressure sensing (when available)
 * - Proper authorization flow handling
 * 
 * Must be called on the main thread due to Apple API requirements.
 */
void InternalSensors::Init()
{
  location_manager = [[CLLocationManager alloc] init];
  location_delegate = [[LocationDelegate alloc] init: &listener];
  location_manager.desiredAccuracy =
      kCLLocationAccuracyBestForNavigation;
  location_manager.delegate = location_delegate;
  
#if TARGET_OS_IPHONE
  // Configure location manager for background operation
  /* XCSoar expires the internal GPS fix after 10 seconds without a
     fresh location.  A distance filter allows CoreLocation to stay
     quiet while the GPS is still healthy, which makes the UI fall
     back to "GPS waiting for fix". */
  location_manager.distanceFilter = kCLDistanceFilterNone;
  location_manager.pausesLocationUpdatesAutomatically = NO;
  
  if ([location_manager
      respondsToSelector: @selector(requestWhenInUseAuthorization)]) {
    CLAuthorizationStatus status;
    if (@available(iOS 14.0, *)) {
      status = location_manager.authorizationStatus;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      /* the instance property above is iOS 14+; fall back to the
         deprecated class method on older systems */
      status = [CLLocationManager authorizationStatus];
#pragma clang diagnostic pop
    }
    [location_delegate handleAuthorizationStatus:status
                                         manager:location_manager];
  } else {
    [location_manager startUpdatingLocation];
  }
    
  // Initialize altimeter to nullptr before any barometer checks
  altimeter = nullptr;
  
  // Check if the device supports barometric pressure sensing
  if ([CMAltimeter isRelativeAltitudeAvailable]) {
    // Check for authorization status (iOS 8+)
    if ([CMAltimeter respondsToSelector:@selector(authorizationStatus)]) {
      CMAuthorizationStatus status = [CMAltimeter authorizationStatus];
      
      // Exit if user denied permission
      if (status == CMAuthorizationStatusDenied) {
        altimeter = nullptr;
        return;
      }
      // Handle case where permission hasn't been determined yet
      else if (status == CMAuthorizationStatusNotDetermined &&
              [CMMotionActivityManager respondsToSelector:@selector(isActivityAvailable)]) {
        // Create persistent manager and queue to check permissions
        motion_activity_manager = [[CMMotionActivityManager alloc] init];
        motion_activity_queue = [[NSOperationQueue alloc] init];
        
        // Query motion activity to trigger permission dialog
        [motion_activity_manager queryActivityStartingFromDate:[NSDate date]
                                        toDate:[NSDate date]
                                       toQueue:motion_activity_queue
                                   withHandler:^(NSArray<CMMotionActivity *> * _Nullable activities, NSError * _Nullable error) {
         (void) activities;
            if (error) {
                NSLog(@"Error querying motion activities: %@", error);
                // Schedule main-thread work for error handling
                dispatch_async(dispatch_get_main_queue(), ^{
                  // Ensure altimeter remains nullptr on error
                  altimeter = nullptr;
                  // Clear the persistent references since we're done
                  motion_activity_manager = nullptr;
                  motion_activity_queue = nullptr;
                });
                return;
            }
            
            // Schedule main-thread work for successful permission grant
            dispatch_async(dispatch_get_main_queue(), ^{
              // Clear the persistent references since we're done with permission check
              motion_activity_manager = nullptr;
              motion_activity_queue = nullptr;
              
              // Only initialize altimeter if permission query succeeded
              StartAltimeterUpdates();
            });
        }];
        
        return; // Exit early since altimeter initialization is handled in the completion block
      }
    }
    
    // Initialize altimeter for pressure readings (for authorized status)
    StartAltimeterUpdates();
    } else {
      // Device doesn't support barometric pressure sensing
      altimeter = nullptr;
    }
    
#else
  [location_manager startUpdatingLocation];
#endif
}

/**
 * Clean up and stop all sensor operations.
 * 
 * Stops:
 * - Location updates from CoreLocation
 * - Barometric pressure updates from CoreMotion (iOS)
 * 
 * Safe to call multiple times. Must be called on main thread.
 */
void InternalSensors::Deinit()
{
  [location_manager stopUpdatingLocation];
  #if TARGET_OS_IPHONE
  if (altimeter != nullptr) {
    [altimeter stopRelativeAltitudeUpdates];
  }
  
  // Clean up persistent motion activity manager and queue
  motion_activity_manager = nullptr;
  motion_activity_queue = nullptr;
  #endif
}

#if TARGET_OS_IPHONE
/**
 * Initialize barometric pressure sensing using iOS CoreMotion framework.
 * 
 * Creates CMAltimeter instance and starts relative altitude updates on a background queue.
 * Converts pressure readings from kilopascals (kPa) to hectopascals (hPa/mbar) 
 * and forwards to the SensorListener interface.
 * 
 * @note Only available on iOS devices with barometric sensors
 * @note Requires Motion & Fitness permission if not already granted
 * @note Pressure values are converted: kPa * 10.0 = hPa/mbar
 */
void InternalSensors::StartAltimeterUpdates()
{
  // Initialize altimeter for pressure readings
  altimeter = [[CMAltimeter alloc] init];
  NSOperationQueue *queue = [[NSOperationQueue alloc] init];
  
  // Start receiving altimeter updates
  [altimeter startRelativeAltitudeUpdatesToQueue:queue
                                     withHandler:^(CMAltitudeData * _Nullable altitudeData, NSError * _Nullable error) {
    if (error) {
      NSLog(@"Error: %@", [error localizedDescription]);
      return;
    }

    // Convert pressure readings (from kPa to hPa/mbar) and notify listener
    listener.OnBarometricPressureSensor(
      static_cast<float>(altitudeData.pressure.floatValue * 10.0f),
      0.0f
    );
  }];
}
#endif

#endif // __APPLE__
